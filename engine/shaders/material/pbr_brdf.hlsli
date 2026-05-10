// =============================================================================
// pbr_brdf.hlsli
// MARS Material — GGX / Disney Principled BRDF helpers.
// =============================================================================

#ifndef MARS_PBR_BRDF_HLSLI
#define MARS_PBR_BRDF_HLSLI

#include "../common/math.hlsli"
#include "../common/random.hlsli"

// ---------------------------------------------------------------------------
// GGX / Trowbridge-Reitz normal distribution function (NDF)
// D(h) = α² / (π · ((NdotH)² · (α²−1) + 1)²)
// ---------------------------------------------------------------------------
float GGX_NDF(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / (PI * d * d);
}

// ---------------------------------------------------------------------------
// Smith G1 for GGX (Heitz 2014 height-correlated)
// ---------------------------------------------------------------------------
float Smith_G1_GGX(float NdotV, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float denom = NdotV + sqrt(a2 + (1.0f - a2) * NdotV * NdotV);
    return 2.0f * NdotV / denom;
}

// Height-correlated Smith G2 masking-shadowing for GGX
// G2(l,v,h) = 1 / (1 + Λ(v) + Λ(l))
float Smith_G2_GGX(float NdotL, float NdotV, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float lambdaV = NdotL * sqrt(a2 + (1.0f - a2) * NdotV * NdotV);
    float lambdaL = NdotV * sqrt(a2 + (1.0f - a2) * NdotL * NdotL);
    return 2.0f * NdotL * NdotV / (lambdaV + lambdaL);
}

// ---------------------------------------------------------------------------
// Schlick Fresnel approximation
// ---------------------------------------------------------------------------
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// ---------------------------------------------------------------------------
// Disney diffuse lobe (Burley 2012)
// Fd = (baseColor/π) · (1 + (FD90−1)·(1−NdotL)^5) · (1 + (FD90−1)·(1−NdotV)^5)
// FD90 = 0.5 + 2·roughness·(HdotL)²
// ---------------------------------------------------------------------------
float3 DisneyDiffuse(float3 baseColor, float roughness,
                     float NdotL, float NdotV, float HdotL)
{
    float FD90   = 0.5f + 2.0f * roughness * HdotL * HdotL;
    float FL     = 1.0f + (FD90 - 1.0f) * pow(saturate(1.0f - NdotL), 5.0f);
    float FV     = 1.0f + (FD90 - 1.0f) * pow(saturate(1.0f - NdotV), 5.0f);
    return baseColor * INV_PI * FL * FV;
}

// ---------------------------------------------------------------------------
// Full specular BRDF: GGX D · Smith G2 · Fresnel / (4 NdotL NdotV)
// Returns the specular lobe value (not multiplied by NdotL — caller does it).
// ---------------------------------------------------------------------------
float3 GGX_Specular(float3 F0, float roughness,
                    float NdotH, float NdotL, float NdotV, float HdotV)
{
    float  D  = GGX_NDF(NdotH, roughness);
    float  G  = Smith_G2_GGX(NdotL, NdotV, roughness);
    float3 F  = FresnelSchlick(HdotV, F0);
    return (D * G * F) / max(4.0f * NdotL * NdotV, 1e-4f);
}

// ---------------------------------------------------------------------------
// Evaluate the full PBR BRDF (diffuse + specular) for a known direction pair.
// Returns radiance contribution: BRDF · NdotL
// ---------------------------------------------------------------------------
float3 EvaluatePBR(float3 baseColor, float metallic, float roughness,
                   float3 N, float3 V, float3 L)
{
    float3 H     = SafeNormalize(V + L);
    float NdotL  = saturate(dot(N, L));
    float NdotV  = saturate(dot(N, V));
    float NdotH  = saturate(dot(N, H));
    float HdotL  = saturate(dot(H, L));
    float HdotV  = saturate(dot(H, V));

    if (NdotL <= 0.0f || NdotV <= 0.0f)
        return float3(0, 0, 0);

    // Dielectric F0 = 0.04, metal F0 = baseColor
    float3 F0      = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);
    float3 kD      = (1.0f - metallic) * (float3(1,1,1) - FresnelSchlick(HdotV, F0));

    float3 diffuse  = kD * DisneyDiffuse(baseColor, roughness, NdotL, NdotV, HdotL);
    float3 specular = GGX_Specular(F0, roughness, NdotH, NdotL, NdotV, HdotV);

    return (diffuse + specular) * NdotL;
}

// ---------------------------------------------------------------------------
// GGX VNDF importance sampling (Heitz 2018)
// Returns a half-vector sampled proportional to the visible normal distribution.
// u1, u2: uniform random numbers in [0,1)
// ---------------------------------------------------------------------------
float3 SampleGGX_VNDF(float3 Ve, float roughness, float u1, float u2)
{
    float alpha = roughness * roughness;
    // Transform view direction to hemisphere configuration
    float3 Vh = SafeNormalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z));
    // Build orthonormal basis
    float3 T1 = (Vh.z < 0.9999f) ? normalize(cross(float3(0,0,1), Vh))
                                  : float3(1,0,0);
    float3 T2 = cross(Vh, T1);
    // Sample disk
    float r = sqrt(u1);
    float phi = TWO_PI * u2;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s  = 0.5f * (1.0f + Vh.z);
    t2 = (1.0f - s) * sqrt(1.0f - t1 * t1) + s * t2;
    // Reconstruct normal
    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0f, 1.0f - t1*t1 - t2*t2)) * Vh;
    // Transform back
    float3 Ne = SafeNormalize(float3(alpha * Nh.x, alpha * Nh.y, max(0.0f, Nh.z)));
    return Ne;
}

#endif // MARS_PBR_BRDF_HLSLI
