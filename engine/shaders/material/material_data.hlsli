// =============================================================================
// material_data.hlsli
// MARS Material — bindless material fetch helpers.
//
// Each draw/instance carries a root constant "material index" that is used
// to look up the per-material structured buffer entry and sample its textures
// from the global bindless descriptor heap.
// =============================================================================

#ifndef MARS_MATERIAL_DATA_HLSLI
#define MARS_MATERIAL_DATA_HLSLI

#include "../common/bindless.hlsli"

// ---------------------------------------------------------------------------
// GpuMaterialData — matches the CPU-side layout uploaded by ResourceManager.
// All texture fields are bindless SRV slot indices (UINT32_MAX = not bound).
// ---------------------------------------------------------------------------
struct GpuMaterialData
{
    float4   base_color_factor;           // linear RGBA
    float    metallic_factor;
    float    roughness_factor;
    float    emissive_scale;
    float    alpha_cutoff;

    uint     base_color_tex;              // bindless SRV index or UINT32_MAX
    uint     normal_tex;
    uint     metallic_roughness_tex;      // G = roughness, B = metallic (glTF convention)
    uint     emissive_tex;

    float3   emissive_factor;
    uint     flags;                       // bit 0: double-sided, bit 1: alpha-masked
};

// GpuInstanceData — per-instance transform data (one entry per TLAS instance)
struct GpuInstanceData
{
    float4x4 world_transform;
    float4x4 world_transform_inv_transpose;
    uint     material_index;             // index into g_MaterialBuffer SRV
    uint     vertex_buffer_srv;          // bindless SRV for vertex StructuredBuffer
    uint     index_buffer_srv;           // bindless SRV for index ByteAddressBuffer
    uint     prev_vertex_buffer_srv;     // previous-frame positions ByteAddressBuffer (cloth); UINT32_MAX = none
    float    lod_alpha;                  // Stochastic LOD opacity in [0,1] — AnyHit rejects when rand >= lod_alpha
    uint     impostor_atlas_srv;         // Octahedral impostor atlas SRV slot (LOD 3); UINT32_MAX = not an impostor
    uint     impostor_view_count;        // View-grid resolution per octahedral axis (e.g. 16 -> 16x16 atlas)
    uint     impostor_depth_normal_srv;  // Depth/normal atlas: RG=oct-normal, B=depth, A=roughness; UINT32_MAX = none
    float3   impostor_aabb_min;          // Object-space AABB min (must match the BLAS procedural AABB)
    uint     _impostor_pad1;
    float3   impostor_aabb_max;          // Object-space AABB max (must match the BLAS procedural AABB)
    uint     _impostor_pad2;
};

// Bindless structured buffer containing all materials for this frame
StructuredBuffer<GpuMaterialData> g_MaterialBuffer[] : register(t0, space2);

// Bindless structured buffer containing all instance transforms
StructuredBuffer<GpuInstanceData> g_InstanceBuffer[] : register(t0, space3);

// Root constant indices (shared between all pipeline states in this engine)
static const uint kRootMaterialBufferSlot  = 0;   // SRV slot of g_MaterialBuffer entry
static const uint kRootInstanceBufferSlot  = 1;   // SRV slot of g_InstanceBuffer entry
static const uint kRootFrameConstantSlot   = 2;   // index into per-frame CB

// ---------------------------------------------------------------------------
// FetchMaterial — retrieve material data for the given material index.
// `materialBufferSlot` is the bindless SRV slot for the frame's material buffer.
// ---------------------------------------------------------------------------
GpuMaterialData FetchMaterial(uint materialBufferSlot, uint materialIndex)
{
    return g_MaterialBuffer[materialBufferSlot][materialIndex];
}

// ---------------------------------------------------------------------------
// ResolveBaseColor — sample the base-color texture (or use the factor fallback).
// ---------------------------------------------------------------------------
float4 ResolveBaseColor(GpuMaterialData mat, float2 uv)
{
    if (mat.base_color_tex != 0xFFFFFFFFu)
        return SampleTextureLevel0(mat.base_color_tex, uv) * mat.base_color_factor;
    return mat.base_color_factor;
}

// ---------------------------------------------------------------------------
// ResolveNormal — sample the normal map and return a world-space perturbed normal.
// `T` and `B` are tangent and bitangent in world space.
// Returns the original geometric normal if no normal map is bound.
// ---------------------------------------------------------------------------
float3 ResolveNormal(GpuMaterialData mat, float2 uv,
                     float3 N, float3 T, float3 B)
{
    if (mat.normal_tex == 0xFFFFFFFFu)
        return N;
    float3 nts = SampleTextureLevel0(mat.normal_tex, uv).xyz * 2.0f - 1.0f;
    return normalize(nts.x * T + nts.y * B + nts.z * N);
}

// ---------------------------------------------------------------------------
// ResolveMetallicRoughness — returns (metallic, roughness) from texture or factors.
// ---------------------------------------------------------------------------
float2 ResolveMetallicRoughness(GpuMaterialData mat, float2 uv)
{
    if (mat.metallic_roughness_tex != 0xFFFFFFFFu)
    {
        float4 s = SampleTextureLevel0(mat.metallic_roughness_tex, uv);
        return float2(s.b * mat.metallic_factor, s.g * mat.roughness_factor);
    }
    return float2(mat.metallic_factor, mat.roughness_factor);
}

// ---------------------------------------------------------------------------
// ResolveEmissive — returns linear emissive radiance.
// ---------------------------------------------------------------------------
float3 ResolveEmissive(GpuMaterialData mat, float2 uv)
{
    if (mat.emissive_tex != 0xFFFFFFFFu)
        return SampleTextureLevel0(mat.emissive_tex, uv).rgb * mat.emissive_factor * mat.emissive_scale;
    return mat.emissive_factor * mat.emissive_scale;
}

#endif // MARS_MATERIAL_DATA_HLSLI
