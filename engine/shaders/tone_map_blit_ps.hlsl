// #profile ps_6_6
// =============================================================================
// tone_map_blit_ps.hlsl
// MARS 3D Engine — tone-map blit pixel shader.
//
// Reads the path-tracer RGBA16F output UAV (as an SRV) and writes to the
// swap-chain back buffer with the appropriate tone-map / color-space transform.
//
// Root constants (32-bit):
//   b0 c0: src_texture_slot  (index into g_Textures[] bindless heap)
//   b0 c1: hdr_mode          (0 = SDR, 1 = HDR10/PQ, 2 = scRGB)
// =============================================================================

#include "common/bindless.hlsli"

// Per-blit root constants
struct BlitConstants
{
    uint src_texture_slot;
    uint hdr_mode;          // 0 = SDR, 1 = HDR10/PQ, 2 = scRGB
};

ConstantBuffer<BlitConstants> g_Blit : register(b0, space0);

// ---------------------------------------------------------------------------
// Color-space helpers
// ---------------------------------------------------------------------------

// Simple Reinhard tone-mapper (HDR linear → SDR [0,1])
float3 reinhard(float3 hdr)
{
    return hdr / (hdr + 1.0f);
}

// sRGB gamma encode (for SDR UNORM output)
float3 linear_to_srgb(float3 c)
{
    return pow(saturate(c), 1.0f / 2.2f);
}

// ACES approximate filmic tone-mapper (matches common game use)
float3 aces_film(float3 x)
{
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// ST.2084 (PQ) EOTF inverse (linear → PQ for HDR10 UNORM output)
float3 linear_to_pq(float3 lin)
{
    // Normalise for 1000 nits peak
    lin = lin / 10000.0f * 203.0f; // scRGB 1.0 = 80 nits; HDR10 normalised to 10000 nits
    lin = max(0.0f, lin);

    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 4096.0f * 32.0f;
    const float c3 = 2392.0f / 4096.0f * 32.0f;

    float3 Ym = pow(lin, m1);
    return pow((c1 + c2 * Ym) / (1.0f + c3 * Ym), m2);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
struct VsOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(VsOut i) : SV_Target
{
    // Sample the path-tracer HDR output (linear RGBA16F)
    float4 hdr = g_Textures[g_Blit.src_texture_slot].SampleLevel(g_SamplerPoint, i.uv, 0);

    float3 color = hdr.rgb;

    if (g_Blit.hdr_mode == 2u)
    {
        // scRGB: swap-chain is RGBA16F — write linear scene-referred values.
        // No tone mapping; the OS compositor handles HDR display.
        return float4(color, 1.0f);
    }
    else if (g_Blit.hdr_mode == 1u)
    {
        // HDR10/PQ: apply ACES, then ST.2084 encode into R10G10B10A2_UNORM.
        color = aces_film(color);
        color = linear_to_pq(color);
        return float4(saturate(color), 1.0f);
    }
    else
    {
        // SDR: Reinhard + sRGB gamma.
        color = reinhard(color);
        color = linear_to_srgb(color);
        return float4(color, 1.0f);
    }
}
