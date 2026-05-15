// #profile cs_6_6
// =============================================================================
// vegetation_wind.hlsl  --  MARS GPU vegetation wind deformation
//
// Implements a physically-motivated three-layer hierarchical wind model.
//
// ── Layer 1: Primary Trunk Bend ──────────────────────────────────────────────
//   The trunk is treated as a fixed-base cantilever beam under a distributed
//   horizontal load.  Displacement follows the analytic beam deflection curve:
//
//       d(h) = (3h² − h³) / 2
//
//   which enforces zero displacement AND zero slope at the root (h = 0) and
//   maximum displacement at the crown (h = 1).  The resulting S-curved arc is
//   how real trunks bend under wind — they curve, not rotate rigidly at the
//   base.
//
//   The driving amplitude is NOT the instantaneous wind speed.  Instead it is
//   `trunk_envelope`, a damped second-order spring output computed on the CPU
//   (natural frequency ≈ 0.22 Hz, ζ ≈ 0.28).  This gives the trunk mass-like
//   inertia: it builds up over ~3-4 s when a gust arrives and decays equally
//   slowly when it subsides, with a small overshoot from the underdamped
//   response.  Trunks are slow to react to wind changes.
//
// ── Layer 2: Branch Sway ─────────────────────────────────────────────────────
//   Branches respond to localised wind drag and turbulence.  The motion is
//   computed procedurally: four inharmonic sinusoids at different frequencies
//   and spatial phases are summed to approximate the stochastic, aperiodic
//   pattern of natural turbulence.  Each vertex's world position seeds its own
//   unique phase so adjacent branches are never in synchrony.
//
//   Two orthogonal displacement components (along-wind and cross-wind) produce
//   the elliptical orbital paths real branches trace.  Branches react faster
//   than the trunk — they use `wind_t_raw` (the instantaneous normalised wind
//   speed) so their amplitude responds within one frame.
//
// ── Layer 3: Leaf Detail Bending ─────────────────────────────────────────────
//   Individual leaf vertices ripple and flutter via a combination of high-
//   frequency sinusoids and a vertex-position noise seed.  Two uncorrelated
//   oscillators at frequencies ~9.5 Hz and ~14.3 Hz are mixed with unequal
//   weights to break periodicity.  The flutter is present even in calm
//   conditions (wind_t_raw floor = 0.15) and uses a sqrt curve so there is
//   always visible motion.  Leaves react almost instantly to wind changes.
//
// ── Phase Variation ──────────────────────────────────────────────────────────
//   `wind_phase_offset` is a per-species pseudorandom offset (derived on the
//   CPU from a golden-ratio integer hash of the species index) that ensures
//   different tree species are never animated in synchronised lockstep.
//   Within a single tree, all three layers derive additional spatial phase
//   variation from vertex world-position coordinates, so branches and leaves
//   within the same mesh are individually differentiated.
//
// ── Scaling ──────────────────────────────────────────────────────────────────
//   wind_strength is in m/s.  Beaufort 3 (gentle breeze) ≈ 5 m/s; Beaufort 9
//   (strong gale) ≈ 20 m/s.  All amplitudes are normalised against 20 m/s and
//   scaled by mesh_height so proportional deformation is constant regardless
//   of tree size.
//
// =============================================================================

#pragma pack_matrix(row_major)

struct WindConstants
{
	uint   vertex_count;
	uint   source_vertex_buffer_srv;   // ByteAddressBuffer @ t0,space1
	uint   output_vertex_buffer_uav;   // RWByteAddressBuffer @ u0,space3
	float  mesh_min_y;                 // Used to normalise height in the tree

	float3 wind_direction;             // Normalised world-space wind direction
	float  wind_strength;              // Wind speed, m/s

	float  time_seconds;               // Global time (loops every ~6553s)
	float  wind_phase_offset;          // Per-species/per-instance phase offset
	float  primary_bend;               // Species trunk bend strength  [0..1]
	float  secondary_sway;             // Species branch sway strength [0..1]

	float  leaf_flutter;               // Species leaf micro-flutter strength [0..1]
	float  mesh_height;                // Height of the mesh AABB (for normalisation)
	uint   prev_pos_buffer_uav;        // RWByteAddressBuffer for compact float3 prev positions
	float  trunk_envelope;            // Spring-mass output from CPU [0..~1.5]: inertia-smoothed wind_t
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
	// Snapshot the current output position into the prev-pos buffer BEFORE
	// computing the new deformed position. The closest-hit shader reads this
	// compact float3 buffer (stride 12) to generate per-vertex motion vectors
	// for the denoiser / TAA reprojection. On the very first frame the output
	// buffer was seeded with rest-pose data by enable_wind_deform(), so the
	// first-frame motion vector is correctly zero.
	// -------------------------------------------------------------------------
	{
		const float3 cur_out_pos = asfloat(
			g_OutputBuffers[g_Wind.output_vertex_buffer_uav].Load3(vid * k_vertex_stride));
		const uint prev_base = vid * 12u;
		g_OutputBuffers[g_Wind.prev_pos_buffer_uav].Store3(prev_base, asuint(cur_out_pos));
	}

	// -------------------------------------------------------------------------
	// Normalised height in tree, [0, 1]: roots = 0, crown = 1.
	// -------------------------------------------------------------------------
	const float h = saturate((v.position.y - g_Wind.mesh_min_y) /
							  max(g_Wind.mesh_height, 1e-3));

	// -------------------------------------------------------------------------
	// Wind strength normalised to [0, 1] against a 20 m/s gale reference.
	// wind_t_raw: instantaneous, used by Layer 2 (branches) and Layer 3 (leaves)
	//             which are fast-reacting by design.
	// trunk_env:  CPU spring-mass output; gives Layer 1 (trunk) its inertia.
	//             Builds up / decays over ~3-4 s; may slightly overshoot.
	// -------------------------------------------------------------------------
	const float wind_t_raw = saturate(g_Wind.wind_strength / 20.0);
	const float trunk_env  = saturate(g_Wind.trunk_envelope);

	// -------------------------------------------------------------------------
	// Time arguments.
	//   t_slow  — used for trunk and branch fundamentals; increases mildly with
	//             wind so the tree doesn't spin like a top at high speed.
	//   t_fast  — used for high-frequency branch harmonics and leaf flutter.
	// Both incorporate wind_phase_offset so species are never in sync.
	// -------------------------------------------------------------------------
	const float speed_mod = 0.5 + wind_t_raw * 0.2;
	const float t_slow = g_Wind.time_seconds * speed_mod       + g_Wind.wind_phase_offset;
	const float t_fast = g_Wind.time_seconds * speed_mod * 3.1 + g_Wind.wind_phase_offset;

	// -------------------------------------------------------------------------
	// Cross-wind axis — perpendicular to wind direction in the horizontal plane.
	// Gives branch sway its elliptical orbital character.
	// -------------------------------------------------------------------------
	const float3 wind_right = normalize(float3(-g_Wind.wind_direction.z, 0.0,
												g_Wind.wind_direction.x));

	// -------------------------------------------------------------------------
	// Off-axis distance from the tree's vertical axis, normalised.
	//   0 → trunk core   (only Layer 1 applied here)
	//   1 → branch tip   (all three layers applied)
	// -------------------------------------------------------------------------
	const float xz_dist  = length(v.position.xz);
	const float off_axis = saturate(xz_dist * 0.35);   // 0=trunk core, 1=branch tip

	// =========================================================================
	// Layer 1: PRIMARY TRUNK BEND
	//
	// Cantilever beam deflection: d(h) = (3h² − h³) / 2
	//   h=0: d=0, d'=0  (root is clamped — no rigid rotation at the base)
	//   h=1: d=1         (crown has maximum displacement)
	//
	// Two inharmonic trunk oscillators are summed for a subtle figure-8 sway
	// rather than a pure back-and-forth pendulum.
	//
	// Amplitude driven by trunk_env (spring output) — NOT raw wind speed — so
	// the trunk reacts slowly and with natural mass-like inertia.
	// =========================================================================
	const float trunk_curve     = (3.0 * h * h - h * h * h) * 0.5;
	const float trunk_freq      = 0.7 + trunk_env * 0.25;         // Hz: slow primary sway
	const float trunk_osc       = sin(t_slow * trunk_freq * 6.28318)
								+ 0.18 * sin(t_slow * trunk_freq * 6.28318 * 2.13 + 0.9);
	const float trunk_amplitude = trunk_curve
								* g_Wind.primary_bend
								* trunk_env
								* max(g_Wind.mesh_height, 1.0) * 0.12;

	float3 displacement = g_Wind.wind_direction * trunk_osc * trunk_amplitude;

	// =========================================================================
	// Layer 2: BRANCH SWAY
	//
	// Procedural stochastic motion: four inharmonic sinusoids at different
	// frequencies and vertex-position-seeded spatial phases are summed to
	// approximate the aperiodic turbulent patterns real branches exhibit.
	//
	// Vertex world-position is used as a unique phase seed so each branch
	// vertex has its own oscillation phase — no two branches move identically.
	//
	// Two orthogonal components (along-wind + cross-wind) produce the elliptical
	// orbital paths seen in nature.  Weight on cross-wind is ~45% of along-wind.
	//
	// Uses wind_t_raw: branches respond almost instantly to gusts, but because
	// the trunk lags (spring), you see branches move first, trunk follows —
	// the correct hierarchical order.
	// =========================================================================
	const float branch_h    = saturate((h - 0.15) * (1.0 / 0.85));  // zero below 15% height
	const float branch_mask = branch_h * off_axis;

	// Four inharmonic frequencies (ratios are irrational → never fully repeat).
	const float vx = v.position.x;
	const float vz = v.position.z;

	// Along-wind: primary + three harmonics at inharmonic ratios.
	const float a0 = sin(t_slow * 2.20 * 6.28318 + vx * 0.93 + vz * 0.71);
	const float a1 = sin(t_slow * 3.67 * 6.28318 + vx * 1.41 + vz * 0.53 + 0.8);
	const float a2 = sin(t_fast * 1.13 * 6.28318 + vx * 2.07 + vz * 1.89 + 2.1);
	const float a3 = sin(t_fast * 1.73 * 6.28318 + vx * 0.61 + vz * 2.33 + 4.2);
	const float along_osc = a0 * 0.45 + a1 * 0.28 + a2 * 0.17 + a3 * 0.10;

	// Cross-wind: independent seeds — uncorrelated from along-wind.
	const float c0 = sin(t_slow * 2.81 * 6.28318 + vx * 0.67 + vz * 1.19 + 1.3);
	const float c1 = sin(t_slow * 4.11 * 6.28318 + vx * 1.53 + vz * 0.87 + 3.0);
	const float c2 = sin(t_fast * 1.47 * 6.28318 + vx * 2.91 + vz * 1.37 + 0.5);
	const float cross_osc = c0 * 0.50 + c1 * 0.30 + c2 * 0.20;

	const float along_amp = g_Wind.secondary_sway * wind_t_raw * 0.55;
	const float cross_amp = g_Wind.secondary_sway * wind_t_raw * 0.25;

	displacement += g_Wind.wind_direction * along_osc * along_amp * branch_mask;
	displacement += wind_right            * cross_osc * cross_amp * branch_mask;

	// =========================================================================
	// Layer 3: LEAF DETAIL BENDING
	//
	// Individual leaf vertices ripple and flutter using two uncorrelated high-
	// frequency oscillators mixed with unequal weights to avoid periodicity.
	// The vertex position seeds a unique phase per leaf so they are never in
	// synchrony — each appears to flutter independently.
	//
	// A sqrt curve on wind_t_raw (+ 0.15 floor) keeps leaves alive in calm air
	// and avoids the harsh on/off look of a linear threshold.
	//
	// Applied only in the upper canopy (h > 0.55) AND to off-axis vertices
	// (off_axis > 0.5) to prevent trunk geometry from vibrating.
	//
	// Leaves react almost instantly: wind_t_raw is used directly, so a gust
	// produces leaf flutter within a single frame.
	// =========================================================================
	const float flutter_mask = saturate((h - 0.55) * 3.0) * saturate(off_axis * 2.0);
	const float flutter_t    = sqrt(saturate(wind_t_raw + 0.15));

	const float fp_a = v.position.x * 5.3 + v.position.y * 3.9 + v.position.z * 4.7;
	const float fp_b = v.position.x * 7.1 + v.position.y * 5.3 + v.position.z * 6.9;

	// Two inharmonic oscillators (ratio 9.5 : 14.3 ≈ 2 : 3, but with irrational
	// frequency scaling so the combined waveform never exactly repeats).
	const float flutter_a = sin(t_fast * 9.5  * 6.28318 + fp_a);
	const float flutter_b = sin(t_fast * 14.3 * 6.28318 + fp_b + 2.4);

	const float flutter_amp = g_Wind.leaf_flutter * flutter_t * 0.14;

	// Displace in XZ plane (tangential flutter) with a smaller Y component
	// (vertical ripple — the leaf tilting up and down as it flutters).
	displacement.x += (flutter_a * 0.65 + flutter_b * 0.35) * flutter_amp * flutter_mask;
	displacement.y += (flutter_a * 0.20)                    * flutter_amp * flutter_mask;
	displacement.z += (cos(t_fast * 9.5  * 6.28318 + fp_a)  * 0.65
					+  cos(t_fast * 14.3 * 6.28318 + fp_b + 2.4) * 0.35)
					* flutter_amp * flutter_mask;

	// =========================================================================
	// Write output vertex (position only deformed; normals/tangents stay at
	// rest-pose — small-angle approximation is acceptable for foliage).
	// =========================================================================
	v.position += displacement;

	StoreVertex(g_OutputBuffers[g_Wind.output_vertex_buffer_uav], vid, v);
}
