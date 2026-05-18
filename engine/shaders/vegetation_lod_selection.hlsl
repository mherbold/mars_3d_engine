// #profile cs_6_6
// =============================================================================
// vegetation_lod_selection.hlsl  --  MARS GPU-driven vegetation LOD selection
//
// Reads the vegetation instance buffer produced by vegetation_placement.hlsl
// and updates each instance's `current_lod` based on its world-space distance
// to the camera. Per-species LOD-distance thresholds are supplied as a small
// structured buffer indexed by species_index.
//
// Stochastic dithering: a per-instance dither value combined with a per-frame
// jitter is used to bias LOD transitions, so adjacent instances at the LOD
// boundary cross the boundary at slightly different distances. This avoids
// a sharp visible LOD seam without needing alpha-blended LODs.
// =============================================================================

#pragma pack_matrix(row_major)

struct LodSelectionConstants
{
	float3 camera_position;       // World-space camera position
	uint   instance_count;        // Number of valid instances in the buffer

	uint   species_count;
	uint   instance_buffer_uav;   // Bindless UAV slot for VegetationInstanceGpu[]
	uint   species_buffer_srv;    // Bindless SRV slot for SpeciesGpu[]
	uint   frame_jitter_seed;     // Per-frame seed for dither jitter

	float  dither_band_meters;    // Width of the dither band around each LOD threshold
	uint   _pad0;
	uint   _pad1;
	uint   _pad2;
};

// Mirror of the placement shader's instance struct
struct VegetationInstanceGpu
{
	float4 position_scale;
	float4 rotation;
	uint   species_index;
	uint   current_lod;
	uint   tlas_instance;
	float  wind_phase_offset;
	float  lod_dither;
	uint   _pad0;
	uint   _pad1;
	uint   _pad2;
};

// Per-species LOD distance thresholds (CPU upload). Layout matches SpeciesDesc
// and k_species_gpu_stride (32 bytes = 8 floats).
struct SpeciesGpu
{
	float lod_near_max;       // LOD 0 -> 1
	float lod_mid_max;        // LOD 1 -> 2
	float lod_far_max;        // LOD 2 -> 3
	float max_draw_distance;  // beyond this, instance is culled
	float bounding_radius;    // conservative world-space bounding sphere radius
	float _pad0;
	float _pad1;
	float _pad2;
};

RWStructuredBuffer<VegetationInstanceGpu> g_RWInstanceBuffers[] : register(u0, space1);
StructuredBuffer<SpeciesGpu>              g_SpeciesBuffers[]    : register(t0, space2);

cbuffer LodCB : register(b0)
{
	LodSelectionConstants g_Constants;
};

// PCG hash — kept for future use (wind phase, dither seeding, etc.)
uint Hash(uint seed)
{
	uint state = seed * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float Hash01(uint seed)
{
	return float(Hash(seed)) / 4294967296.0;
}

// VegetationLOD enum values
#define LOD_NEAR        0u
#define LOD_MID         1u
#define LOD_FAR_CLUSTER 2u
#define LOD_IMPOSTOR    3u
#define LOD_CULLED      0xFFFFFFFFu

[numthreads(64, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
	const uint idx = dispatch_thread_id.x;
	if (idx >= g_Constants.instance_count)
		return;

	VegetationInstanceGpu inst = g_RWInstanceBuffers[g_Constants.instance_buffer_uav][idx];

	const uint species_idx = min(inst.species_index, g_Constants.species_count - 1u);
	const SpeciesGpu sp = g_SpeciesBuffers[g_Constants.species_buffer_srv][species_idx];

	// Distance from camera to instance (XZ-weighted full 3D distance)
	const float3 to_inst = inst.position_scale.xyz - g_Constants.camera_position;
	const float  distance = length(to_inst);

	// Per-instance stable dither offset spreads LOD transitions across space so
	// nearby instances don't all switch at the same hard distance ring.
	// frame_jitter is intentionally NOT included here: adding a fresh random
	// per-frame offset to the threshold distance causes per-frame LOD flickering
	// for any instance sitting near a boundary.
	const float dither_offset = (inst.lod_dither - 0.5f) * g_Constants.dither_band_meters;

	const float d = distance + dither_offset;

	uint lod;
	if (d < sp.lod_near_max)         lod = LOD_NEAR;
	else if (d < sp.lod_mid_max)     lod = LOD_MID;
	else if (d < sp.lod_far_max)     lod = LOD_FAR_CLUSTER;
	else if (d < sp.max_draw_distance) lod = LOD_IMPOSTOR;
	else                             lod = LOD_CULLED;

	inst.current_lod = lod;
	g_RWInstanceBuffers[g_Constants.instance_buffer_uav][idx] = inst;
}
