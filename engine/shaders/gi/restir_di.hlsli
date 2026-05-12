// =============================================================================
// restir_di.hlsli
// MARS GI — ReSTIR Direct Illumination shared data structures and math.
//
// This header contains only the pure data types and math helpers for ReSTIR DI
// that are independent of DXR-specific types (TraceRay, ShadowPayload, etc.).
// The DXR-dependent functions (RIS_GenerateCandidates, ReservoirEvaluate) live
// in path_trace.hlsl where ShadowPayload and the DXR intrinsics are in scope.
//
// Reference: "Spatiotemporal reservoir resampling for real-time ray tracing
// with dynamic direct lighting" — Bitterli et al., SIGGRAPH 2020.
// =============================================================================

#ifndef MARS_RESTIR_DI_HLSLI
#define MARS_RESTIR_DI_HLSLI

#include "../common/math.hlsli"
#include "../common/random.hlsli"

// ---------------------------------------------------------------------------
// Reservoir — stores the selected light sample and its accumulated weight.
//
//  y_direction : selected light direction (world space, normalised)
//  y_radiance  : selected light radiance
//  w_sum       : running sum of un-normalised weights during resampling
//  W           : final unbiased contribution weight = w_sum / (M * p_hat(y))
//  M           : number of candidates that have streamed through this reservoir
// ---------------------------------------------------------------------------
struct Reservoir
{
    float3 y_direction;
    float3 y_radiance;
    float  w_sum;
    float  W;
    uint   M;
};

// Initialise an empty reservoir.
Reservoir ReservoirInit()
{
    Reservoir r;
    r.y_direction = float3(0, 1, 0);
    r.y_radiance  = float3(0, 0, 0);
    r.w_sum       = 0.0f;
    r.W           = 0.0f;
    r.M           = 0u;
    return r;
}

// Stream one candidate through the reservoir.
// Returns true if the new sample was accepted.
bool ReservoirUpdate(inout Reservoir r, float3 dir, float3 rad, float w, inout uint rng)
{
    r.w_sum += w;
    r.M     += 1u;
    if (PcgRand(rng) * r.w_sum <= w)
    {
        r.y_direction = dir;
        r.y_radiance  = rad;
        return true;
    }
    return false;
}

// Finalise: compute the unbiased contribution weight W.
void ReservoirFinalize(inout Reservoir r, float p_hat)
{
    r.W = (p_hat > 0.0f) ? r.w_sum / (float(r.M) * p_hat) : 0.0f;
}

// ---------------------------------------------------------------------------
// Light descriptor — M7 supports a single directional (sun) light.
// ---------------------------------------------------------------------------
struct DirectionalLight
{
    float3 direction;    // world-space direction toward the light (normalised)
    float3 radiance;     // pre-multiplied color × intensity
};

// ---------------------------------------------------------------------------
// Target PDF p_hat — proportional to unshadowed luminance contribution:
//   p_hat(x) = mean(|BRDF|) · NdotL · luminance(radiance)
// ---------------------------------------------------------------------------
float PdfHat(float3 brdf_val, float NdotL, float3 radiance)
{
    float brdf_mean = dot(brdf_val, float3(1.0f/3.0f, 1.0f/3.0f, 1.0f/3.0f));
    float lum       = dot(radiance, float3(0.2126f, 0.7152f, 0.0722f));
    return max(brdf_mean, 0.0f) * NdotL * lum;
}

#endif // MARS_RESTIR_DI_HLSLI
