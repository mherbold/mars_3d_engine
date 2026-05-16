// #profile cs_6_6
// =============================================================================
// vegetation_wind.hlsl  --  MARS GPU vegetation procedural deformation
//
// Implements a pure time-based two-layer procedural motion model.
// There is no wind direction or physics simulation; all motion is driven by
// elapsed time and per-species/per-vertex deterministic parameters.
//
// ── Layer 1: Primary Trunk Bend ──────────────────────────────────────────────
//   The trunk bends with a constant amplitude (primary_bend_strength) whose
//   direction rotates in a complete circle over primary_bend_circle_time
//   seconds.  Deflection follows the analytic cantilever beam curve:
//
//       d(h) = (3h² − h³) / 2
//
//   which enforces zero displacement AND zero slope at the root (h = 0) and
//   maximum displacement at the crown (h = 1).
//
// ── Layer 2: Leaf Detail Flutter ─────────────────────────────────────────────
//   Individual leaf vertices ripple via two uncorrelated high-frequency
//   sinusoids seeded from vertex world-position.  The flutter is constant-
//   amplitude; leaf_flutter_strength controls its magnitude and
//   leaf_flutter_speed controls its rate.
//
// =============================================================================

#pragma pack_matrix(row_major)

struct WindConstants
{
	uint   vertex_count;
	uint   source_vertex_buffer_srv;   // ByteAddressBuffer @ t0,space1
	uint   output_vertex_buffer_uav;   // RWByteAddressBuffer @ u0,space3
	float  mesh_min_y;                 // Used to normalise height in the tree

	float  time_seconds;               // Global time (loops every ~6553s)
	float  phase_offset;               // Per-species pseudorandom phase offset (radians)
	float  primary_bend_strength;      // Trunk bend amplitude  [0..1]
	float  primary_bend_speed;         // Trunk bend rotation speed (rotations per second)

	float  leaf_flutter_strength;      // Leaf micro-flutter amplitude [0..1]
	float  leaf_flutter_speed;         // Leaf flutter speed multiplier [0..]
	float  mesh_height;                // Height of the mesh AABB (for normalisation)
	uint   prev_pos_buffer_uav;        // RWByteAddressBuffer for compact float3 prev positions
	uint   is_leaf_mesh;               // 1 = leaf/frond submesh

	uint   _pad0;
	uint   _pad1;
	uint   _pad2;
};

ByteAddressBuffer    g_Buffers[]        : register(t0, space1);
RWByteAddressBuffer  g_OutputBuffers[]  : register(u0, space3);

cbuffer WindCB : register(b0)
{
	WindConstants g_Wind;
};

static const uint k_vertex_stride = 76u;

struct GpuVertex
{
	float3 position;
	float3 normal;
	float3 tangent;
	float2 uv;
	uint4  bone_indices;
	float4 bone_weights;
};

GpuVertex LoadVertex(ByteAddressBuffer buffer, uint index)
{
	uint base = index * k_vertex_stride;
	GpuVertex v;
	v.position     = asfloat(buffer.Load3(base +  0u));
	v.normal       = asfloat(buffer.Load3(base + 12u));
	v.tangent      = asfloat(buffer.Load3(base + 24u));
	v.uv           = asfloat(buffer.Load2(base + 36u));
	v.bone_indices = buffer.Load4(base + 44u);
	v.bone_weights = asfloat(buffer.Load4(base + 60u));
	return v;
}

void StoreVertex(RWByteAddressBuffer buffer, uint index, GpuVertex v)
{
	uint base = index * k_vertex_stride;
	buffer.Store3(base +  0u, asuint(v.position));
	buffer.Store3(base + 12u, asuint(v.normal));
	buffer.Store3(base + 24u, asuint(v.tangent));
	buffer.Store2(base + 36u, asuint(v.uv));
	buffer.Store4(base + 44u, v.bone_indices);
	buffer.Store4(base + 60u, asuint(v.bone_weights));
}

[numthreads(64, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
	const uint vid = dispatch_thread_id.x;
	if (vid >= g_Wind.vertex_count)
		return;

	GpuVertex v = LoadVertex(g_Buffers[g_Wind.source_vertex_buffer_srv], vid);

	// -------------------------------------------------------------------------
	// Snapshot current output position into prev-pos buffer for motion vectors.
	// -------------------------------------------------------------------------
	{
		const float3 cur_out_pos = asfloat(
			g_OutputBuffers[g_Wind.output_vertex_buffer_uav].Load3(vid * k_vertex_stride));
		const uint prev_base = vid * 12u;
		g_OutputBuffers[g_Wind.prev_pos_buffer_uav].Store3(prev_base, asuint(cur_out_pos));
	}

	// -------------------------------------------------------------------------
	// Normalised height in tree [0, 1].
	// -------------------------------------------------------------------------
	const float h = saturate((v.position.y - g_Wind.mesh_min_y) /
							  max(g_Wind.mesh_height, 1e-3));

	float3 displacement = float3(0.0, 0.0, 0.0);

	// =========================================================================
	// Layer 1: PRIMARY TRUNK BEND (circular, time-based)
	//
	// The bend direction rotates once around the Y axis every
	// primary_bend_circle_time seconds.  Amplitude is constant and equals
	// primary_bend_strength scaled by mesh height.
	// Deflection follows the cantilever curve d(h) = (3h² − h³) / 2.
	// =========================================================================
	const float trunk_curve = (3.0 * h * h - h * h * h) * 0.5;

	const float bend_angle = g_Wind.time_seconds * max(g_Wind.primary_bend_speed, 0.0) * 6.28318
							+ g_Wind.phase_offset;

	const float bend_amp = trunk_curve
						 * g_Wind.primary_bend_strength
						 * max(g_Wind.mesh_height, 1.0) * 0.12;

	displacement.x += cos(bend_angle) * bend_amp;
	displacement.z += sin(bend_angle) * bend_amp;

	// =========================================================================
	// Layer 2: LEAF DETAIL FLUTTER
	//
	// Two uncorrelated high-frequency sinusoids seeded from vertex position.
	// Applied to leaf/frond submeshes (is_leaf_mesh) at all heights, or to
	// upper-canopy bark geometry above h > 0.55.
	// =========================================================================
	const float xz_dist  = length(v.position.xz);
	const float off_axis = saturate(xz_dist * 0.35);

	const float flutter_mask = (g_Wind.is_leaf_mesh != 0u)
		? saturate(off_axis * 2.0)
		: saturate((h - 0.55) * 3.0) * saturate(off_axis * 2.0);

	const float t_fast = g_Wind.time_seconds * 1.4 * max(g_Wind.leaf_flutter_speed, 0.0) + g_Wind.phase_offset;

	// High-frequency spatial seeds: large, incommensurate multipliers on all
	// three axes so that leaves only a few centimetres apart differ
	// significantly in phase.  The Y component deliberately uses a much
	// higher and unrelated multiplier so height bands don't stay in-phase.
	const float fp_a = v.position.x * 17.3 + v.position.z * 13.7 + v.position.y * 29.1;
	const float fp_b = v.position.x * 23.9 + v.position.z * 19.1 + v.position.y * 11.7;

	const float flutter_a = sin(t_fast * 2.3 * 6.28318 + fp_a);
	const float flutter_b = sin(t_fast * 3.5 * 6.28318 + fp_b + 2.4);

	const float flutter_amp = g_Wind.leaf_flutter_strength * 0.10;

	displacement.x += (flutter_a * 0.65 + flutter_b * 0.35) * flutter_amp * flutter_mask;
	displacement.y += (flutter_a * 0.15)                    * flutter_amp * flutter_mask;
	displacement.z += (cos(t_fast * 2.3 * 6.28318 + fp_a) * 0.65
					+  cos(t_fast * 3.5 * 6.28318 + fp_b + 2.4) * 0.35)
					* flutter_amp * flutter_mask;

	// =========================================================================
	// Write output vertex.
	// =========================================================================
	v.position += displacement;

	StoreVertex(g_OutputBuffers[g_Wind.output_vertex_buffer_uav], vid, v);
}
