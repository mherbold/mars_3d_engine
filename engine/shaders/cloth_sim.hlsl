// #profile cs_6_6
// =============================================================================
// cloth_sim.hlsl  --  MARS GPU cloth simulation (canonical XPBD)
//
// Three-position-state layout:
//   pos_prev  (x_{n-1}) -- position from the step before last
//   pos_curr  (x_n)     -- last committed position
//   pos_pred  (x*)      -- working buffer during integrate / constrain
//
// Velocity is implicit:  v_n = (pos_curr - pos_prev) / dt
// No separate velocity buffer is needed or used.
//
// Per-substep dispatch sequence (issued by path_tracer.cpp):
//
//   1. PASS_INTEGRATE (once)
//      Verlet prediction:
//        x* = x_curr + (x_curr - x_prev) * damping + a * dt^2
//      Pinned particles: x* = x_curr.
//      Writes pos_pred_a.
//
//   2. PASS_CONSTRAIN (xpbd_iterations times)
//      Jacobi XPBD distance constraints.
//      Reads constrain_read_srv, writes constrain_write_uav.
//      C++ alternates pred_a/pred_b each iteration (UAV barrier between each).
//      Also writes the GpuVertex buffer every iteration (last write wins).
//
//   3. PASS_FINALIZE (once)
//      State rotation:
//        new pos_prev = old pos_curr
//        new pos_curr = final x* (from constrain_read_srv)
//      Writes pos_prev_uav and pos_curr_uav.
// =============================================================================

#pragma pack_matrix(row_major)

struct ClothConstants
{
	uint    grid_w;
	uint    grid_h;
	uint    vertex_count;

	float   delta_time;

	float3  gravity;
	float   inv_mass;

	float3  wind;       // per-instance wind direction (from ClothDesc.wind_direction)
	float   damping;

	float   structural_compliance;
	float   shear_compliance;
	float   bend_compliance;

	float   rest_len_struct;
	float   rest_len_shear;
	float   rest_len_bend;

	// 3-position-state XPBD bindless slots
	uint    pos_prev_srv;
	uint    pos_curr_srv;
	uint    pos_prev_uav;
	uint    pos_curr_uav;

	uint    pos_pred_a_uav;
	uint    output_vertex_uav;

	uint    sim_pass;

	uint    constrain_read_srv;
	uint    constrain_write_uav;

	// pin_corners bitmask
	//   Bit 0 = top-left     Bit 1 = top-right
	//   Bit 2 = bottom-left  Bit 3 = bottom-right
	//   Bit 4 = top edge     Bit 5 = bottom edge
	//   Bit 6 = left edge    Bit 7 = right edge
	uint    pin_corners;

	float   wave_amplitude;  // procedural wave injection amplitude (from ClothDesc.wave_amplitude)
	float   time_seconds;    // global elapsed time
};

ConstantBuffer<ClothConstants> g_Cloth : register(b0, space0);

ByteAddressBuffer     g_Buffers[]   : register(t0, space1);
RWByteAddressBuffer   g_RWBuffers[] : register(u0, space3);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
float3 load_f3(ByteAddressBuffer buf, uint i)   { return asfloat(buf.Load3(i * 12u)); }
void   store_f3(RWByteAddressBuffer buf, uint i, float3 v) { buf.Store3(i * 12u, asuint(v)); }

static const uint k_vtx_stride = 76u;
void write_vertex(RWByteAddressBuffer buf, uint i, float3 pos, float3 nrm, float3 tan)
{
	uint b = i * k_vtx_stride;
	buf.Store3(b +  0u, asuint(pos));
	buf.Store3(b + 12u, asuint(nrm));
	buf.Store3(b + 24u, asuint(tan));
}

float particle_inv_mass(uint col, uint row)
{
	uint lc = g_Cloth.grid_w - 1u;
	uint lr = g_Cloth.grid_h - 1u;
	bool tl = (col == 0u  && row == 0u ) && (g_Cloth.pin_corners &   1u);
	bool tr = (col == lc  && row == 0u ) && (g_Cloth.pin_corners &   2u);
	bool bl = (col == 0u  && row == lr ) && (g_Cloth.pin_corners &   4u);
	bool br = (col == lc  && row == lr ) && (g_Cloth.pin_corners &   8u);
	bool te = (row == 0u )               && (g_Cloth.pin_corners &  16u);
	bool be = (row == lr )               && (g_Cloth.pin_corners &  32u);
	bool le = (col == 0u )               && (g_Cloth.pin_corners &  64u);
	bool re = (col == lc )               && (g_Cloth.pin_corners & 128u);
	return (tl || tr || bl || br || te || be || le || re) ? 0.0f : g_Cloth.inv_mass;
}

float3 xpbd_corr(float3 pi, float3 pj, float wi, float wj,
				 float rest, float alpha, inout float lam)
{
	float3 d   = pi - pj;
	float  len = max(length(d), 1e-6f);
	float  C   = len - rest;
	float  den = wi + wj + alpha;
	if (den < 1e-10f) return (float3)0;
	float dl = -(C + alpha * lam) / den;
	lam += dl;
	return wi * dl * (d / len);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	uint idx = tid.x;
	if (idx >= g_Cloth.vertex_count) return;

	uint col = idx % g_Cloth.grid_w;
	uint row = idx / g_Cloth.grid_w;

	// =========================================================================
	// PASS 0 -- INTEGRATE
	// =========================================================================
	if (g_Cloth.sim_pass == 0u)
	{
		float3 xp = load_f3(g_Buffers[g_Cloth.pos_prev_srv], idx);
		float3 xc = load_f3(g_Buffers[g_Cloth.pos_curr_srv], idx);
		float  w  = particle_inv_mass(col, row);

		float3 pred;
		if (w > 0.0f)
		{
			float3 vel   = (xc - xp) * g_Cloth.damping;
			float3 accel = g_Cloth.gravity + g_Cloth.wind;
			pred = xc + vel + accel * (g_Cloth.delta_time * g_Cloth.delta_time);
		}
		else
		{
			pred = xc;
		}

		store_f3(g_RWBuffers[g_Cloth.pos_pred_a_uav], idx, pred);
		return;
	}

	// =========================================================================
	// PASS 1 -- CONSTRAIN (Jacobi XPBD)
	// =========================================================================
	if (g_Cloth.sim_pass == 1u)
	{
		ByteAddressBuffer   src  = g_Buffers[g_Cloth.constrain_read_srv];
		RWByteAddressBuffer dst  = g_RWBuffers[g_Cloth.constrain_write_uav];

		float3 p = load_f3(src, idx);
		float  w = particle_inv_mass(col, row);

		if (w <= 0.0f)
		{
			// Pinned: restore anchor and emit render vertex.
			p = load_f3(g_Buffers[g_Cloth.pos_curr_srv], idx);
			store_f3(dst, idx, p);

			uint li = (col > 0u)                  ? idx - 1u             : idx;
			uint ri = (col + 1u < g_Cloth.grid_w) ? idx + 1u             : idx;
			uint ui = (row > 0u)                   ? idx - g_Cloth.grid_w : idx;
			uint di = (row + 1u < g_Cloth.grid_h)  ? idx + g_Cloth.grid_w : idx;
			float3 dX = load_f3(src, ri) - load_f3(src, li);
			float3 dY = load_f3(src, di) - load_f3(src, ui);
			write_vertex(g_RWBuffers[g_Cloth.output_vertex_uav], idx, p,
						 normalize(cross(dX, dY)), normalize(dX));
			return;
		}

		float dt2 = g_Cloth.delta_time * g_Cloth.delta_time;
		float as  = g_Cloth.structural_compliance / dt2;
		float ash = g_Cloth.shear_compliance      / dt2;
		float ab  = g_Cloth.bend_compliance       / dt2;

		float3 sc = (float3)0; float sn = 0.0f;
		float3 hc = (float3)0; float hn = 0.0f;
		float3 bc = (float3)0; float bn = 0.0f;
		float lam = 0.0f;

		// structural right
		if (col + 1u < g_Cloth.grid_w)
		{ float3 pj=load_f3(src,idx+1u); float wj=particle_inv_mass(col+1u,row);
		  sc+=xpbd_corr(p,pj,w,wj,g_Cloth.rest_len_struct,as,lam); lam=0.0f; sn+=1.0f; }
		// structural left
		if (col > 0u)
		{ float3 pj=load_f3(src,idx-1u); float wj=particle_inv_mass(col-1u,row);
		  sc+=xpbd_corr(p,pj,w,wj,g_Cloth.rest_len_struct,as,lam); lam=0.0f; sn+=1.0f; }
		// structural down
		if (row + 1u < g_Cloth.grid_h)
		{ float3 pj=load_f3(src,idx+g_Cloth.grid_w); float wj=particle_inv_mass(col,row+1u);
		  sc+=xpbd_corr(p,pj,w,wj,g_Cloth.rest_len_struct,as,lam); lam=0.0f; sn+=1.0f; }
		// structural up
		if (row > 0u)
		{ float3 pj=load_f3(src,idx-g_Cloth.grid_w); float wj=particle_inv_mass(col,row-1u);
		  sc+=xpbd_corr(p,pj,w,wj,g_Cloth.rest_len_struct,as,lam); lam=0.0f; sn+=1.0f; }

		// shear down-right
		if (col+1u<g_Cloth.grid_w && row+1u<g_Cloth.grid_h)
		{ float3 pj=load_f3(src,idx+g_Cloth.grid_w+1u); float wj=particle_inv_mass(col+1u,row+1u);
		  hc+=xpbd_corr(p,pj,w,wj,g_Cloth.rest_len_shear,ash,lam); lam=0.0f; hn+=1.0f; }
		// shear down-left
		if (col>0u && row+1u<g_Cloth.grid_h)
		{ float3 pj=load_f3(src,idx+g_Cloth.grid_w-1u); float wj=particle_inv_mass(col-1u,row+1u);
		  hc+=xpbd_corr(p,pj,w,wj,g_Cloth.rest_len_shear,ash,lam); lam=0.0f; hn+=1.0f; }
		// shear up-right
		if (col+1u<g_Cloth.grid_w && row>0u)
		{ float3 pj=load_f3(src,idx-g_Cloth.grid_w+1u); float wj=particle_inv_mass(col+1u,row-1u);
		  hc+=xpbd_corr(p,pj,w,wj,g_Cloth.rest_len_shear,ash,lam); lam=0.0f; hn+=1.0f; }
		// shear up-left
		if (col>0u && row>0u)
		{ float3 pj=load_f3(src,idx-g_Cloth.grid_w-1u); float wj=particle_inv_mass(col-1u,row-1u);
		  hc+=xpbd_corr(p,pj,w,wj,g_Cloth.rest_len_shear,ash,lam); lam=0.0f; hn+=1.0f; }

		// bend skip-one right
		if (col+2u<g_Cloth.grid_w)
		{ float3 pj=load_f3(src,idx+2u); float wj=particle_inv_mass(col+2u,row);
		  bc+=xpbd_corr(p,pj,w,wj,g_Cloth.rest_len_bend,ab,lam); lam=0.0f; bn+=1.0f; }
		// bend skip-one left
		if (col>=2u)
		{ float3 pj=load_f3(src,idx-2u); float wj=particle_inv_mass(col-2u,row);
		  bc+=xpbd_corr(p,pj,w,wj,g_Cloth.rest_len_bend,ab,lam); lam=0.0f; bn+=1.0f; }
		// bend skip-one down
		if (row+2u<g_Cloth.grid_h)
		{ float3 pj=load_f3(src,idx+2u*g_Cloth.grid_w); float wj=particle_inv_mass(col,row+2u);
		  bc+=xpbd_corr(p,pj,w,wj,g_Cloth.rest_len_bend,ab,lam); lam=0.0f; bn+=1.0f; }
		// bend skip-one up
		if (row>=2u)
		{ float3 pj=load_f3(src,idx-2u*g_Cloth.grid_w); float wj=particle_inv_mass(col,row-2u);
		  bc+=xpbd_corr(p,pj,w,wj,g_Cloth.rest_len_bend,ab,lam); lam=0.0f; bn+=1.0f; }

		p += (sn>0.0f ? sc/sn : (float3)0)
		   + (hn>0.0f ? hc/hn : (float3)0)
		   + (bn>0.0f ? bc/bn : (float3)0);

		store_f3(dst, idx, p);

		// Write render vertex every iteration; last pass wins.
		{
			uint li = (col > 0u)                  ? idx - 1u             : idx;
			uint ri = (col + 1u < g_Cloth.grid_w) ? idx + 1u             : idx;
			uint ui = (row > 0u)                   ? idx - g_Cloth.grid_w : idx;
			uint di = (row + 1u < g_Cloth.grid_h)  ? idx + g_Cloth.grid_w : idx;
			float3 dX = load_f3(src, ri) - load_f3(src, li);
			float3 dY = load_f3(src, di) - load_f3(src, ui);
			write_vertex(g_RWBuffers[g_Cloth.output_vertex_uav], idx, p,
						 normalize(cross(dX, dY)), normalize(dX));
		}
		return;
	}

	// =========================================================================
	// PASS 2 -- FINALIZE  (state rotation)
	// =========================================================================
	if (g_Cloth.sim_pass == 2u)
	{
		float3 xc   = load_f3(g_Buffers[g_Cloth.pos_curr_srv],          idx);
		float3 xnew = load_f3(g_Buffers[g_Cloth.constrain_read_srv],    idx);

		store_f3(g_RWBuffers[g_Cloth.pos_prev_uav], idx, xc);
		store_f3(g_RWBuffers[g_Cloth.pos_curr_uav], idx, xnew);
		return;
	}

	// =========================================================================
	// PASS 3 -- WAVE  (procedural traveling-wave injection into pos_curr)
	//
	// Injects a sine-wave offset into the committed position buffer BEFORE
	// the next INTEGRATE pass reads it.  This gives cloth a persistent wave
	// motion that is independent of the XPBD solver.
	//
	// The wave travels along the column axis with a spatial frequency derived
	// from the grid width.  Per-row phase seeds produce vertical variation so
	// the cloth appears to ripple rather than oscillate as a rigid sheet.
	// =========================================================================
	if (g_Cloth.sim_pass == 3u)
	{
		float  w = particle_inv_mass(col, row);
		if (w <= 0.0f) return; // pinned particles are immovable

		float3 xc = load_f3(g_Buffers[g_Cloth.pos_curr_srv], idx);

		// Traveling wave: spatial phase from column, temporal from time_seconds.
		// Row seed breaks the wave into horizontal bands for visual richness.
		const float k     = 6.28318 / max((float)g_Cloth.grid_w, 1.0);
		const float phase = k * (float)col - g_Cloth.time_seconds * 2.5
						  + (float)row * 0.4;
		const float wave  = sin(phase) * g_Cloth.wave_amplitude;

		// Displace along the stored per-instance wind direction.
		xc += normalize(g_Cloth.wind + float3(0,0,1e-4)) * wave;

		store_f3(g_RWBuffers[g_Cloth.pos_curr_uav], idx, xc);
		return;
	}
}
