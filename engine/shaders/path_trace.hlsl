// #profile lib_6_3
// =============================================================================
// path_trace.hlsl
// MARS Path Tracer — ray generation, closest hit, and miss shaders.
//
// Shader model: lib_6_3 (DXR ray-tracing library)
// =============================================================================

#pragma pack_matrix(row_major)

#include "common/math.hlsli"
#include "common/random.hlsli"
#include "material/material_data.hlsli"
#include "material/pbr_brdf.hlsli"

// ---------------------------------------------------------------------------
// Per-frame constant buffer (bound at b0, space0)
// ---------------------------------------------------------------------------
struct FrameConstants
{
    float4x4 view_inv;          // inverse view matrix
    float4x4 proj_inv;          // inverse projection matrix
    float3   camera_pos;        // world-space camera position
    uint     frame_index;       // monotonically increasing frame counter
    uint     output_width;
    uint     output_height;
    uint     tlas_slot;         // bindless SRV slot of the TLAS
    uint     material_buffer_slot; // bindless SRV slot for GpuMaterialData[]
    uint     instance_buffer_slot; // bindless SRV slot for GpuInstanceData[]
    uint     output_uav_slot;   // bindless UAV slot for the output texture
    uint     _pad_align0;       // \_ 8 bytes of padding to push sun_direction to the next
    uint     _pad_align1;       // /  16-byte register boundary (arrays would each use 16 bytes)
    float3   sun_direction;     // world-space direction TOWARD the sun
    float    sun_intensity;
    float3   sun_color;         // linear RGB
    float    _pad0;
    float    _pad[12];          // explicit padding to 256 bytes (matches C++ FrameConstants)
};

ConstantBuffer<FrameConstants> g_Frame : register(b0, space0);

// TLAS acceleration structure (bindless via slot in frame constants)
RaytracingAccelerationStructure g_TLAS[] : register(t0, space4);

// Output UAV — RGBA16F (scRGB linear, HDR-ready)
RWTexture2D<float4> g_OutputUAV[] : register(u0, space1);

// ---------------------------------------------------------------------------
// Payload structures
// ---------------------------------------------------------------------------
struct PrimaryPayload
{
    float3 radiance;
    float  hit_t;
    bool   missed;
};

struct ShadowPayload
{
    uint occluded; // 1 = occluded, 0 = unoccluded (bool is unreliable in DXR payloads)
};

// ---------------------------------------------------------------------------
// Vertex fetch helper — reads vertex data from the bindless vertex buffer.
// Interpolates using the built-in barycentrics from the hit attributes.
// ---------------------------------------------------------------------------
struct TriangleVertices
{
    float3 position;
    float3 normal;
    float3 tangent;
    float2 uv;
};

struct GpuVertex
{
    float3 position;
    float3 normal;
    float3 tangent;
    float2 uv;
    uint4  bone_indices;
    float4 bone_weights;
};

TriangleVertices FetchInterpolatedVertex(uint instanceIndex, uint primitiveIndex,
                                         float2 bary)
{
    GpuInstanceData inst = g_InstanceBuffer[g_Frame.instance_buffer_slot][instanceIndex];

    // Index buffer — stored as ByteAddressBuffer in g_Buffers; each index is uint32
    uint baseIdx = primitiveIndex * 3u;
    uint i0 = g_Buffers[inst.index_buffer_srv].Load((baseIdx + 0u) * 4u);
    uint i1 = g_Buffers[inst.index_buffer_srv].Load((baseIdx + 1u) * 4u);
    uint i2 = g_Buffers[inst.index_buffer_srv].Load((baseIdx + 2u) * 4u);

    // Vertex buffer — manual byte fetch from ByteAddressBuffer.
    // GpuVertex layout (must match the C++ struct):
    //   float3 position  @  0 (12 bytes)
    //   float3 normal    @ 12 (12 bytes)
    //   float3 tangent   @ 24 (12 bytes)
    //   float2 uv        @ 36 ( 8 bytes)
    //   uint4  bone_idx  @ 44 (16 bytes)   -- not needed here, skipped
    //   float4 bone_wt   @ 60 (16 bytes)   -- not needed here, skipped
    // stride = 76 bytes
    static const uint k_stride = 76u;
    uint vbSrv = inst.vertex_buffer_srv;

    GpuVertex v0, v1, v2;

    uint base0 = i0 * k_stride;
    v0.position = asfloat(g_Buffers[vbSrv].Load3(base0 +  0u));
    v0.normal   = asfloat(g_Buffers[vbSrv].Load3(base0 + 12u));
    v0.tangent  = asfloat(g_Buffers[vbSrv].Load3(base0 + 24u));
    v0.uv       = asfloat(g_Buffers[vbSrv].Load2(base0 + 36u));
    v0.bone_indices = (uint4)0; v0.bone_weights = (float4)0;

    uint base1 = i1 * k_stride;
    v1.position = asfloat(g_Buffers[vbSrv].Load3(base1 +  0u));
    v1.normal   = asfloat(g_Buffers[vbSrv].Load3(base1 + 12u));
    v1.tangent  = asfloat(g_Buffers[vbSrv].Load3(base1 + 24u));
    v1.uv       = asfloat(g_Buffers[vbSrv].Load2(base1 + 36u));
    v1.bone_indices = (uint4)0; v1.bone_weights = (float4)0;

    uint base2 = i2 * k_stride;
    v2.position = asfloat(g_Buffers[vbSrv].Load3(base2 +  0u));
    v2.normal   = asfloat(g_Buffers[vbSrv].Load3(base2 + 12u));
    v2.tangent  = asfloat(g_Buffers[vbSrv].Load3(base2 + 24u));
    v2.uv       = asfloat(g_Buffers[vbSrv].Load2(base2 + 36u));
    v2.bone_indices = (uint4)0; v2.bone_weights = (float4)0;

    float w0 = 1.0f - bary.x - bary.y;
    float w1 = bary.x;
    float w2 = bary.y;

    TriangleVertices tri;
    tri.position = v0.position * w0 + v1.position * w1 + v2.position * w2;
    tri.normal   = normalize(v0.normal   * w0 + v1.normal   * w1 + v2.normal   * w2);
    tri.tangent  = normalize(v0.tangent  * w0 + v1.tangent  * w1 + v2.tangent  * w2);
    tri.uv       = v0.uv * w0 + v1.uv * w1 + v2.uv * w2;
    return tri;
}

// Transform a normal from object to world space using the stored inverse-transpose.
float3 TransformNormal(float3 n, float4x4 invTranspose)
{
    return normalize(mul((float3x3)invTranspose, n));
}

// ---------------------------------------------------------------------------
// [shader("raygeneration")]  RayGen
// Fires one primary ray per pixel.  The ray direction is reconstructed from
// the inverse view-projection matrices stored in the per-frame CB.
// ---------------------------------------------------------------------------
[shader("raygeneration")]
void RayGen()
{
    uint2 launchIdx  = DispatchRaysIndex().xy;
    uint2 launchDim  = DispatchRaysDimensions().xy;

    if (launchIdx.x >= g_Frame.output_width || launchIdx.y >= g_Frame.output_height)
        return;

    // Sub-pixel jitter via Halton sequence for temporal accumulation
    float2 jitter = HaltonJitter(g_Frame.frame_index);
    float2 pixel  = (float2(launchIdx) + jitter) / float2(launchDim);

    // Reconstruct ray origin and direction from inverse VP matrices
    float2 ndc     = pixel * 2.0f - 1.0f;
    ndc.y          = -ndc.y;   // flip Y (D3D NDC has +Y up)

    float4 rayOriginH = mul(g_Frame.view_inv, float4(0, 0, 0, 1));
    float4 rayTargetH = mul(g_Frame.proj_inv, float4(ndc, 1, 1));
    float3 rayTarget  = mul(g_Frame.view_inv, float4(rayTargetH.xyz, 0)).xyz;

    RayDesc ray;
    ray.Origin    = rayOriginH.xyz;
    ray.Direction = normalize(rayTarget);
    ray.TMin      = 1e-4f;
    ray.TMax      = 1e6f;

    PrimaryPayload payload;
    payload.radiance = float3(0, 0, 0);
    payload.hit_t    = -1.0f;
    payload.missed   = false;

    TraceRay(g_TLAS[g_Frame.tlas_slot],
             0,              // RAY_FLAG_NONE
             0xFF,           // instance mask
             0,              // hit group index
             1,              // contribution to hit group index
             0,              // miss shader index
             ray,
             payload);

    g_OutputUAV[g_Frame.output_uav_slot][launchIdx] = float4(payload.radiance, 1.0f);
}

// ---------------------------------------------------------------------------
// [shader("miss")]  Miss — procedural orientation skybox
//
// Each cube face is colour-coded and overlaid with an 8×8 UV grid and a
// centre crosshair so the viewer can immediately tell which direction they
// are looking.
//
// Face → colour mapping:
//   +X  East    red       (0.80, 0.15, 0.15)
//   −X  West    orange    (0.80, 0.45, 0.10)
//   +Y  Up      white     (0.90, 0.90, 0.90)
//   −Y  Down    brown     (0.25, 0.15, 0.05)
//   +Z  South   blue      (0.15, 0.40, 0.80)
//   −Z  North   green     (0.15, 0.70, 0.25)
// ---------------------------------------------------------------------------
[shader("miss")]
void Miss(inout PrimaryPayload payload)
{
    float3 dir = normalize(WorldRayDirection());
    float  ax  = abs(dir.x);
    float  ay  = abs(dir.y);
    float  az  = abs(dir.z);

    float2 uv;
    float3 faceColor;

    if (ax >= ay && ax >= az)
    {
        float inv = 0.5f / ax;
        if (dir.x > 0.0f)
        {
            uv        = float2(-dir.z, -dir.y) * inv + 0.5f;  // +X  East
            faceColor = float3(0.80f, 0.15f, 0.15f);
        }
        else
        {
            uv        = float2( dir.z, -dir.y) * inv + 0.5f;  // -X  West
            faceColor = float3(0.80f, 0.45f, 0.10f);
        }
    }
    else if (ay >= ax && ay >= az)
    {
        float inv = 0.5f / ay;
        if (dir.y > 0.0f)
        {
            uv        = float2( dir.x,  dir.z) * inv + 0.5f;  // +Y  Up
            faceColor = float3(0.90f, 0.90f, 0.90f);
        }
        else
        {
            uv        = float2( dir.x, -dir.z) * inv + 0.5f;  // -Y  Down
            faceColor = float3(0.25f, 0.15f, 0.05f);
        }
    }
    else
    {
        float inv = 0.5f / az;
        if (dir.z > 0.0f)
        {
            uv        = float2( dir.x, -dir.y) * inv + 0.5f;  // +Z  South
            faceColor = float3(0.15f, 0.40f, 0.80f);
        }
        else
        {
            uv        = float2(-dir.x, -dir.y) * inv + 0.5f;  // -Z  North
            faceColor = float3(0.15f, 0.70f, 0.25f);
        }
    }

    // 8×8 UV grid with thin black lines
    const float k_cells    = 8.0f;
    const float k_line     = 0.04f;  // line half-width in cell UV space
    float2 cellUV = frac(uv * k_cells);
    bool   onGrid = cellUV.x < k_line || cellUV.x > (1.0f - k_line)
                 || cellUV.y < k_line || cellUV.y > (1.0f - k_line);

    // White crosshair at the face centre so the exact forward direction is clear
    float2 fromCentre = abs(uv - 0.5f);
    bool   onCross    = (fromCentre.x < 0.006f && fromCentre.y < 0.05f)
                     || (fromCentre.y < 0.006f && fromCentre.x < 0.05f);

    float3 color = onCross ? float3(1.0f, 1.0f, 1.0f)
                 : onGrid  ? float3(0.0f, 0.0f, 0.0f)
                 : faceColor;

    payload.radiance = color;
    payload.hit_t    = -1.0f;
    payload.missed   = true;
}

// ---------------------------------------------------------------------------
// [shader("miss")]  ShadowMiss — ray is not occluded
// ---------------------------------------------------------------------------
[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.occluded = 0u;
}

// ---------------------------------------------------------------------------
// [shader("closesthit")]  ClosestHit — basic PBR shading (single directional light)
// ---------------------------------------------------------------------------
[shader("closesthit")]
void ClosestHit(inout PrimaryPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint instanceIndex  = InstanceIndex();
    uint primitiveIndex = PrimitiveIndex();
    float2 bary         = attr.barycentrics;

    // Fetch interpolated vertex data
    TriangleVertices tri = FetchInterpolatedVertex(instanceIndex, primitiveIndex, bary);

    // Transform to world space
    GpuInstanceData inst      = g_InstanceBuffer[g_Frame.instance_buffer_slot][instanceIndex];
    float3 worldPos           = mul(inst.world_transform, float4(tri.position, 1)).xyz;
    float3 worldNormal        = TransformNormal(tri.normal,   inst.world_transform_inv_transpose);
    float3 worldTangent       = TransformNormal(tri.tangent,  inst.world_transform_inv_transpose);
    float3 worldBitangent     = cross(worldNormal, worldTangent);

    // Fetch material
    GpuMaterialData mat       = FetchMaterial(g_Frame.material_buffer_slot, inst.material_index);

    // Resolve material parameters
    float4 baseColorA         = ResolveBaseColor(mat, tri.uv);
    float3 baseColor          = baseColorA.rgb;
    float2 metalRough         = ResolveMetallicRoughness(mat, tri.uv);
    float  metallic           = metalRough.x;
    float  roughness          = max(metalRough.y, 0.04f);
    float3 N                  = ResolveNormal(mat, tri.uv, worldNormal, worldTangent, worldBitangent);
    float3 emissive           = ResolveEmissive(mat, tri.uv);

    float3 V = normalize(g_Frame.camera_pos - worldPos);
    if (dot(N, V) < 0.0f)
        N = -N;   // flip for double-sided surfaces

    float3 sunDir      = normalize(g_Frame.sun_direction);
    float3 sunRadiance = g_Frame.sun_color * g_Frame.sun_intensity;

    // Shadow ray
    ShadowPayload shadowPayload;
    shadowPayload.occluded = 1u;

    RayDesc shadowRay;
    shadowRay.Origin    = worldPos + N * 1e-3f;
    shadowRay.Direction = sunDir;
    shadowRay.TMin      = 1e-4f;
    shadowRay.TMax      = 1e6f;

    TraceRay(g_TLAS[g_Frame.tlas_slot],
             0x4 | 0x8, // RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
             0xFF,
             0,
             1,
             1,   // ShadowMiss index
             shadowRay,
             shadowPayload);

    float3 directLight = float3(0, 0, 0);
    if (shadowPayload.occluded == 0u)
        directLight = EvaluatePBR(baseColor, metallic, roughness, N, V, sunDir) * sunRadiance;

    // Simple ambient (will be replaced by path-traced GI in M7/M8)
    float3 ambient = baseColor * 0.02f;

    payload.radiance = directLight + ambient + emissive;
    payload.hit_t    = RayTCurrent();
    payload.missed   = false;
}

