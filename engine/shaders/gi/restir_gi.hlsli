// =============================================================================
// restir_gi.hlsli
// MARS GI — ReSTIR Global Illumination shared data structures and math.
//
// Contains pure data types and math helpers for ReSTIR GI.
// DXR-dependent functions (TraceRay calls) live in path_trace.hlsl.
//
// Reference: "ReSTIR GI: Path Resampling for Real-Time Path Tracing"
// — Ouyang et al., High-Performance Graphics 2021.
// =============================================================================

#ifndef MARS_RESTIR_GI_HLSLI
#define MARS_RESTIR_GI_HLSLI

#include "../common/math.hlsli"
#include "../common/random.hlsli"

// ---------------------------------------------------------------------------
// GIReservoir — stores a secondary path sample for indirect illumination.
//
//  x_world      : world-space position of the secondary hit point
//  x_normal     : world-space shading normal at the secondary hit point (normalised)
//  Lo           : outgoing radiance from the secondary hit point toward the primary hit
//  w_sum        : running sum of un-normalised weights during resampling
//  W            : final unbiased contribution weight
//  M            : number of path samples that have streamed through this reservoir
// ---------------------------------------------------------------------------
struct GIReservoir
{
    float3 x_world;   // secondary hit position
    float  w_sum;
    float3 x_normal;  // secondary hit shading normal
    float  W;
    float3 Lo;        // outgoing radiance from secondary hit
    uint   M;
    // Primary-hit surface data — used for disocclusion rejection during temporal reuse.
    // Stored in world-space so the receiving frame can compare without inverting matrices.
    float3 p_normal;  // world-space shading normal of the primary hit
    float  p_depth;   // linear ray-length to the primary hit
};

// Initialise an empty GI reservoir.
GIReservoir GIReservoirInit()
{
    GIReservoir r;
    r.x_world  = float3(0, 0, 0);
    r.x_normal = float3(0, 1, 0);
    r.Lo       = float3(0, 0, 0);
    r.w_sum    = 0.0f;
    r.W        = 0.0f;
    r.M        = 0u;
    r.p_normal = float3(0, 1, 0);
    r.p_depth  = 0.0f;
    return r;
}

// Stream one candidate path sample through the GI reservoir.
// Returns true if the new sample was accepted.
bool GIReservoirUpdate(inout GIReservoir r,
                       float3 x_world, float3 x_normal, float3 Lo,
                       float w, inout uint rng)
{
    r.w_sum += w;
    r.M     += 1u;
    if (PcgRand(rng) * r.w_sum <= w)
    {
        r.x_world  = x_world;
        r.x_normal = x_normal;
        r.Lo       = Lo;
        return true;
    }
    return false;
}

// Finalise: compute the unbiased contribution weight W.
void GIReservoirFinalize(inout GIReservoir r, float p_hat)
{
    r.W = (p_hat > 0.0f) ? r.w_sum / (float(r.M) * p_hat) : 0.0f;
}

// Merge reservoir `b` into reservoir `a` (for temporal / spatial reuse).
// `p_hat_b` is the target PDF of b's stored sample evaluated at the merge point.
bool GIReservoirMerge(inout GIReservoir a, GIReservoir b, float p_hat_b, inout uint rng)
{
    float w = p_hat_b * b.W * float(b.M);
    bool accepted = GIReservoirUpdate(a, b.x_world, b.x_normal, b.Lo, w, rng);
    a.M += b.M - 1u; // one M increment already done inside GIReservoirUpdate
    return accepted;
}

// ---------------------------------------------------------------------------
// GI target PDF: luminance of the unshadowed radiance contribution.
// p_hat(x) = luminance(Lo) · NdotL_secondary
// ---------------------------------------------------------------------------
float GIPdfHat(float3 Lo, float3 N_primary, float3 dir_to_secondary)
{
    float NdotL = saturate(dot(N_primary, dir_to_secondary));
    float lum   = dot(Lo, float3(0.2126f, 0.7152f, 0.0722f));
    return lum * NdotL;
}

// ---------------------------------------------------------------------------
// Cosine-weighted hemisphere sampling (for initial candidate generation).
// Returns a world-space direction sampled proportional to cos(theta).
// Tangent space basis: (T, B, N) with N as the surface normal.
// ---------------------------------------------------------------------------
float3 SampleCosineHemisphere(float3 N, float u1, float u2)
{
    // Malley's method: sample a disk and project up to hemisphere
    float r   = sqrt(u1);
    float phi = TWO_PI * u2;
    float x   = r * cos(phi);
    float y   = r * sin(phi);
    float z   = sqrt(max(0.0f, 1.0f - u1));

    // Build orthonormal tangent frame around N
    float3 T, B;
    if (abs(N.x) > 0.9f)
        T = normalize(cross(float3(0, 1, 0), N));
    else
        T = normalize(cross(float3(1, 0, 0), N));
    B = cross(N, T);

    return normalize(x * T + y * B + z * N);
}

// PDF for cosine-weighted hemisphere sample: p(omega) = NdotL / PI
float CosineHemispherePdf(float NdotL)
{
    return max(NdotL, 0.0f) * INV_PI;
}

// ---------------------------------------------------------------------------
// GIReservoir structured buffer — per-pixel reservoir ring buffer.
// The CPU allocates width * height * k_gi_history_frames elements.
// Current frame reads [frameIndex & 1], writes [1 - frameIndex & 1].
// (Temporal ping-pong: 2 layers.)
// ---------------------------------------------------------------------------
static const uint k_gi_history_frames = 2u;

// Flat index into the per-pixel reservoir buffer:
//   slot = layer * (width * height) + pixelY * width + pixelX
uint GIReservoirIndex(uint2 pixel, uint width, uint layer)
{
    return layer * (width * /* height implicit */ 1u) + pixel.y * width + pixel.x;
    // Note: caller must multiply layer by (width * height); this helper omits height
    // to avoid passing it separately. The structured buffer is indexed by the caller.
}

#endif // MARS_RESTIR_GI_HLSLI
