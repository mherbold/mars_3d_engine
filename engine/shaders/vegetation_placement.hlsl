// #profile cs_6_6
// =============================================================================
// vegetation_placement.hlsl  --  MARS GPU-driven vegetation instance placement
//
// Reads a density map texture and emits VegetationInstance data into a
// structured buffer. Each thread processes one potential spawn location
// on a regular grid within the world bounds.
//
// The density map is sampled at the thread's world-space XZ coordinates,
// and a stochastic test determines whether to spawn an instance at that
// location. Random rotation and scale variation are applied.
//
// Output is written to a RWStructuredBuffer<VegetationInstanceGpu> with
// an atomic counter to track the total number of instances spawned.
// =============================================================================

#pragma pack_matrix(row_major)

// CPU-side constants uploaded each placement dispatch
struct PlacementConstants
{
	float3  world_min;           // World-space bounding box min
	float   placement_y;         // Y-coordinate for all vegetation

	float3  world_max;           // World-space bounding box max
	uint    max_instances;       // Hard cap on total instance count

	float   density_multiplier;  // Global density scale
	uint    species_count;       // Number of species in the species array
	uint    density_map_srv;     // Bindless SRV slot for density texture (R8_UNORM)
	uint    instance_buffer_uav; // Bindless UAV slot for output instance buffer

	uint    instance_counter_uav; // Bindless UAV slot for atomic counter (R32_UINT)
	uint    grid_resolution;      // Number of grid cells per axis (e.g., 512 → 512x512 grid)
	uint    random_seed;          // Frame-unique seed for placement hash
	uint    _pad0;
};

// GPU-side vegetation instance data (matches VegetationInstance CPU struct layout)
struct VegetationInstanceGpu
{
	float4  position_scale;  // xyz = position, w = uniform scale
	float4  rotation;        // quaternion (x, y, z, w)
	uint    species_index;   // Index into species array
	uint    current_lod;     // VegetationLOD enum value (updated by LOD selection pass)
	uint    tlas_instance;   // PathTracer TLAS slot
	float   wind_phase_offset; // Per-instance wind phase offset for variety
	float   lod_dither;        // Stochastic dither value [0, 1) for LOD blending
	uint    _pad0;
	uint    _pad1;
	uint    _pad2;
};

// Bindless heap access
Texture2D<float>             g_Textures[]          : register(t0, space0);
RWStructuredBuffer<uint>     g_RWBuffersU32[]      : register(u0, space0);
RWStructuredBuffer<VegetationInstanceGpu> g_RWInstanceBuffers[] : register(u0, space1);

// Static sampler declared in the placement root signature
SamplerState g_LinearClampSampler : register(s0);

// Root constants (16 DWORDs) — bound at b0 via SetComputeRoot32BitConstants.
cbuffer PlacementCB : register(b0)
{
    PlacementConstants g_Constants;
};

// =============================================================================
// Hash functions (PCG-based)
// =============================================================================
uint Hash(uint seed)
{
	uint state = seed * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float Hash01(uint seed)
{
	return float(Hash(seed)) / 4294967296.0; // 2^32
}

// Hash with multiple inputs
uint Hash3(uint x, uint y, uint z)
{
	return Hash(x + Hash(y + Hash(z)));
}

// =============================================================================
// Main compute shader
// =============================================================================
[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
	const uint grid_x = dispatch_thread_id.x;
	const uint grid_z = dispatch_thread_id.y;

	// Early out if outside grid bounds
	if (grid_x >= g_Constants.grid_resolution || grid_z >= g_Constants.grid_resolution)
		return;

	// Map grid cell to world-space XZ coordinates
	const float3 world_size = g_Constants.world_max - g_Constants.world_min;
	const float2 cell_size = world_size.xz / float(g_Constants.grid_resolution);

	// Per-cell jitter: shift the spawn point randomly within the cell so trees
	// do not sit on a visible regular grid.
	const uint jitter_seed_x = Hash(Hash3(grid_x, grid_z, g_Constants.random_seed) + 6u);
	const uint jitter_seed_z = Hash(Hash3(grid_x, grid_z, g_Constants.random_seed) + 7u);
	const float jitter_x = (Hash01(jitter_seed_x) - 0.5f) * cell_size.x;
	const float jitter_z = (Hash01(jitter_seed_z) - 0.5f) * cell_size.y;

	const float world_x = g_Constants.world_min.x + (grid_x + 0.5f) * cell_size.x + jitter_x;
	const float world_z = g_Constants.world_min.z + (grid_z + 0.5f) * cell_size.y + jitter_z;

	// Sample density map (normalized UV coordinates)
	const float2 uv = float2(grid_x, grid_z) / float(g_Constants.grid_resolution);
	const float density = g_Textures[g_Constants.density_map_srv].SampleLevel(
		g_LinearClampSampler, uv, 0).r;

	// Stochastic spawn test: density × density_multiplier = spawn probability
	const float spawn_probability = saturate(density * g_Constants.density_multiplier);

	const uint spawn_seed = Hash3(grid_x, grid_z, g_Constants.random_seed);
	const float spawn_roll = Hash01(spawn_seed);

	if (spawn_roll >= spawn_probability)
		return; // No spawn at this location

	// Atomic increment to claim an instance slot
	uint instance_index;
	InterlockedAdd(g_RWBuffersU32[g_Constants.instance_counter_uav][0], 1, instance_index);

	// Check if we've exceeded the max instance count
	if (instance_index >= g_Constants.max_instances)
		return;

	// Select species (for now, random; in the future, this could be driven by a species weight map)
	const uint species_seed = Hash(spawn_seed + 1);
	const uint species_index = species_seed % max(g_Constants.species_count, 1u);

	// Random rotation around Y axis (0-360 degrees)
	const uint rotation_seed = Hash(spawn_seed + 2);
	const float rotation_angle = Hash01(rotation_seed) * 6.28318530718; // 2π
	const float half_angle = rotation_angle * 0.5;
	const float4 rotation_quat = float4(0, sin(half_angle), 0, cos(half_angle)); // Y-axis rotation

	// Random scale variation (±20%)
	const uint scale_seed = Hash(spawn_seed + 3);
	const float scale = 0.8 + Hash01(scale_seed) * 0.4; // [0.8, 1.2]

	// Random wind phase offset (0-2π)
	const uint wind_seed = Hash(spawn_seed + 4);
	const float wind_phase_offset = Hash01(wind_seed) * 6.28318530718;

	// Write instance data
	VegetationInstanceGpu instance;
	instance.position_scale = float4(world_x, g_Constants.placement_y, world_z, scale);
	instance.rotation = rotation_quat;
	instance.species_index = species_index;
	instance.current_lod = 0; // Will be updated by LOD selection pass
	instance.tlas_instance = 0xFFFFFFFF; // Will be assigned by TLAS build
	instance.wind_phase_offset = wind_phase_offset;
	instance.lod_dither = Hash01(Hash(spawn_seed + 5));
	instance._pad0 = 0;
	instance._pad1 = 0;
	instance._pad2 = 0;

	g_RWInstanceBuffers[g_Constants.instance_buffer_uav][instance_index] = instance;
}
