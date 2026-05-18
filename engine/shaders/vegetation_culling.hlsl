// #profile cs_6_6
// =============================================================================
// vegetation_culling.hlsl  --  MARS GPU-driven vegetation frustum culling
//
// Reads the vegetation instance buffer and marks instances as visible/culled
// based on frustum and distance tests. Runs before vegetation_lod_selection.hlsl
// each frame to reduce the workload of LOD selection and TLAS construction.
//
// Frustum culling uses the 6 planes extracted from the view-projection matrix.
// Each instance is tested against all 6 planes using its world-space position
// plus a conservative bounding sphere radius (per-species).
//
// Distance culling uses the per-species max_draw_distance threshold.
// =============================================================================

#pragma pack_matrix(row_major)

struct CullingConstants
{
	float4x4 view_proj;           // Current frame view * projection matrix
	float3   camera_position;     // World-space camera position
	uint     instance_count;      // Number of valid instances in the buffer

	uint     instance_buffer_uav; // Bindless UAV slot for VegetationInstanceGpu[]
	uint     species_buffer_srv;  // Bindless SRV slot for SpeciesGpu[]
	uint     species_count;
	uint     _pad0;
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

// Per-species data. Layout matches SpeciesDesc.
struct SpeciesGpu
{
	float lod_near_max;       // LOD 0 -> 1
	float lod_mid_max;        // LOD 1 -> 2
	float lod_far_max;        // LOD 2 -> 3
	float max_draw_distance;  // beyond this, instance is culled
	float bounding_radius;    // Conservative bounding sphere radius for frustum culling
	float _pad0;
	float _pad1;
	float _pad2;
};

RWStructuredBuffer<VegetationInstanceGpu> g_RWInstanceBuffers[] : register(u0, space1);
StructuredBuffer<SpeciesGpu>              g_SpeciesBuffers[]    : register(t0, space2);

cbuffer CullingCB : register(b0)
{
	CullingConstants g_Constants;
};

// VegetationLOD enum values
#define LOD_NEAR        0u
#define LOD_MID         1u
#define LOD_FAR_CLUSTER 2u
#define LOD_IMPOSTOR    3u
#define LOD_CULLED      0xFFFFFFFFu

// =============================================================================
// Extract frustum planes from view-projection matrix
// Returns 6 plane equations in the form (A, B, C, D) where Ax + By + Cz + D >= 0
// means the point is inside the frustum.
// =============================================================================
void ExtractFrustumPlanes(float4x4 view_proj, out float4 planes[6])
{
	// Left plane:   row4 + row1
	planes[0] = view_proj[3] + view_proj[0];

	// Right plane:  row4 - row1
	planes[1] = view_proj[3] - view_proj[0];

	// Bottom plane: row4 + row2
	planes[2] = view_proj[3] + view_proj[1];

	// Top plane:    row4 - row2
	planes[3] = view_proj[3] - view_proj[1];

	// Near plane:   row3
	planes[4] = view_proj[2];

	// Far plane:    row4 - row3
	planes[5] = view_proj[3] - view_proj[2];

	// Normalize all planes
	[unroll]
	for (uint i = 0; i < 6; ++i)
	{
		float len = length(planes[i].xyz);
		if (len > 0.0001)
			planes[i] /= len;
	}
}

// =============================================================================
// Test a sphere against all 6 frustum planes
// Returns true if the sphere is at least partially inside the frustum
// =============================================================================
bool SphereInFrustum(float3 center, float radius, float4 planes[6])
{
	[unroll]
	for (uint i = 0; i < 6; ++i)
	{
		// Signed distance from sphere center to plane
		float dist = dot(planes[i].xyz, center) + planes[i].w;

		// If sphere is entirely on the negative side of any plane, it's culled
		if (dist < -radius)
			return false;
	}

	return true;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
	const uint idx = dispatch_thread_id.x;
	if (idx >= g_Constants.instance_count)
		return;

	VegetationInstanceGpu inst = g_RWInstanceBuffers[g_Constants.instance_buffer_uav][idx];

	// Skip if already culled by previous pass
	if (inst.current_lod == LOD_CULLED)
		return;

	const uint species_idx = min(inst.species_index, g_Constants.species_count - 1u);
	const SpeciesGpu sp = g_SpeciesBuffers[g_Constants.species_buffer_srv][species_idx];

	// Distance culling
	const float3 to_inst = inst.position_scale.xyz - g_Constants.camera_position;
	const float  distance = length(to_inst);

	if (distance > sp.max_draw_distance)
	{
		inst.current_lod = LOD_CULLED;
		g_RWInstanceBuffers[g_Constants.instance_buffer_uav][idx] = inst;
		return;
	}

	// Frustum culling
	float4 frustum_planes[6];
	ExtractFrustumPlanes(g_Constants.view_proj, frustum_planes);

	// Use species bounding radius scaled by instance scale
	const float instance_scale = inst.position_scale.w;
	const float bounding_radius = sp.bounding_radius * instance_scale;

	if (!SphereInFrustum(inst.position_scale.xyz, bounding_radius, frustum_planes))
	{
		inst.current_lod = LOD_CULLED;
		g_RWInstanceBuffers[g_Constants.instance_buffer_uav][idx] = inst;
		return;
	}

	// Instance is visible - leave current_lod unchanged (will be set by LOD selection pass)
}
