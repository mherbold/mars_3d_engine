// =============================================================================
// bindless.hlsli
// MARS Common — bindless descriptor heap access helpers.
//
// All GPU resources (textures, structured buffers, acceleration structures)
// are registered in a single large CBV/SRV/UAV descriptor heap.
// Shaders receive per-draw descriptor indices via root constants.
// =============================================================================

#pragma once

// Bindless heap register space
// Space 0: CBV/SRV/UAV heap (1M slots)
// Space 1: Sampler heap

// Root constant layout (shared between all pipeline states)
// Slot 0: material index  (index into g_MaterialBuffer)
// Slot 1: instance index  (index into g_InstanceBuffer)
// More slots added as systems grow.

// Global bindless texture array (Texture2D)
Texture2D                g_Textures[]    : register(t0, space0);

// Global structured buffer array (raw byte address for heterogeneous data)
ByteAddressBuffer        g_Buffers[]     : register(t0, space1);

// Global RW structured buffer array (UAV)
RWByteAddressBuffer      g_RWBuffers[]   : register(u0, space0);

// Standard trilinear + anisotropic sampler
SamplerState             g_SamplerLinear : register(s0);
SamplerState             g_SamplerPoint  : register(s1);

// Helper: sample a bindless Texture2D
float4 SampleTexture(uint texIndex, float2 uv)
{
    return g_Textures[texIndex].Sample(g_SamplerLinear, uv);
}

// Helper: sample a bindless Texture2D at LOD 0
float4 SampleTextureLevel0(uint texIndex, float2 uv)
{
    return g_Textures[texIndex].SampleLevel(g_SamplerLinear, uv, 0);
}
