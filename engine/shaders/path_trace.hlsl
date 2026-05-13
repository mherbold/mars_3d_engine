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
#include "gi/restir_di.hlsli"
#include "gi/restir_gi.hlsli"

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
    uint     output_uav_slot;         // bindless UAV slot for the output texture   (offset 164)
    uint     motion_vector_uav_slot;  // bindless UAV slot for the motion vector UAV (offset 168)
    uint     depth_uav_slot;          // bindless UAV slot for the linear depth UAV  (offset 172)
    // register 11 — padding to push sun_direction to offset 192
    uint     _pad_align0;             // offset 176
    uint     _pad_align1;             // offset 180
    uint     _pad_align2;             // offset 184
    uint     _pad_align3;             // offset 188
    float3   sun_direction;           // offset 192 — register 12
    float    sun_intensity;
    float3   sun_color;               // offset 208 — register 13
    float    _pad0;
    float4x4 prev_view_proj;          // offset 224 — registers 14..17
    // AOV UAV slots for G-buffer writes (added M7) — offset 288, registers 18..18
    uint     albedo_uav_slot;
    uint     specular_albedo_uav_slot;
    uint     normals_uav_slot;
    uint     roughness_uav_slot;
    // ReSTIR GI (M8) — offset 304, registers 19..19
    uint     gi_reservoir_uav_slot;   // bindless UAV slot for GIReservoir structured buffer
    uint     gi_enabled;              // 1 = trace secondary GI ray in ClosestHit
    // padding — offset 312, registers 20..20
    uint     _pad_gi0;
    uint     _pad_gi1;
    // current-frame view * projection — offset 320, registers 21..24
    float4x4 view_proj;
};

ConstantBuffer<FrameConstants> g_Frame : register(b0, space0);

// TLAS acceleration structure (bindless via slot in frame constants)
RaytracingAccelerationStructure g_TLAS[] : register(t0, space4);

// Output UAV — RGBA16F (scRGB linear, HDR-ready)
RWTexture2D<float4> g_OutputUAV[] : register(u0, space1);

// Motion vector UAV — R16G16B16A16_FLOAT (XY = screen-space pixels delta)
RWTexture2D<float4> g_MotionVectorUAV[] : register(u0, space2);

// Linear depth UAV — R32_FLOAT
RWTexture2D<float>  g_LinearDepthUAV[]  : register(u0, space3);

// AOV UAVs for DLSS-RR G-buffer tagging (M7)
RWTexture2D<float4> g_AlbedoUAV[]         : register(u0, space5);
RWTexture2D<float4> g_SpecularAlbedoUAV[] : register(u0, space6);
RWTexture2D<float4> g_NormalsUAV[]        : register(u0, space7);
RWTexture2D<float>  g_RoughnessUAV[]      : register(u0, space8);

// GI reservoir structured buffer — per-pixel GIReservoir ping-pong (M8)
// Dimensions: width * height * 2 elements (two layers for temporal reuse)
RWStructuredBuffer<GIReservoir> g_GIReservoirs[] : register(u0, space9);

// ---------------------------------------------------------------------------
// Payload structures
// ---------------------------------------------------------------------------
struct PrimaryPayload
{
    float3 radiance;
    float  hit_t;
    uint   missed;      // 1 = missed, 0 = hit (bool unreliable in DXR payloads)
    uint   depth;       // 0 = primary ray, 1 = secondary GI ray (no further recursion)
    float3 sec_normal;  // world-space shading normal at the secondary hit (depth-1 only)
};

struct ShadowPayload
{
    uint occluded; // 1 = occluded, 0 = unoccluded (bool is unreliable in DXR payloads)
};

// ---------------------------------------------------------------------------
// ReSTIR DI — DXR-dependent functions
// These require ShadowPayload, TraceRay, and EvaluatePBR to be in scope, so
// they live here rather than in restir_di.hlsli.
// ---------------------------------------------------------------------------

// RIS initial candidate generation.
// For the single directional sun light in M7, all candidates are identical.
// The structure is already correct for extending to area/emissive lights (M8+).
Reservoir RIS_GenerateCandidates(
    float3 baseColor, float metallic, float roughness,
    float3 N, float3 V,
    DirectionalLight light,
    inout uint rng)
{
    static const uint k_candidates = 8u;
    Reservoir r = ReservoirInit();

    [unroll]
    for (uint i = 0u; i < k_candidates; ++i)
    {
        float3 L     = light.direction;
        float  NdotL = saturate(dot(N, L));

        // Source PDF q(x): uniform over the single light = 1.0
        float  q        = 1.0f;
        float3 brdf_val = EvaluatePBR(baseColor, metallic, roughness, N, V, L);
        float  p_hat    = PdfHat(brdf_val / max(NdotL, 1e-4f), NdotL, light.radiance);
        float  w        = p_hat / max(q, 1e-6f);
        ReservoirUpdate(r, L, light.radiance, w, rng);
    }

    // Finalise with p_hat at the selected sample
    float  NdotL_sel = saturate(dot(N, r.y_direction));
    float3 brdf_sel  = EvaluatePBR(baseColor, metallic, roughness, N, V, r.y_direction);
    float  p_hat_sel = PdfHat(brdf_sel / max(NdotL_sel, 1e-4f), NdotL_sel, r.y_radiance);
    ReservoirFinalize(r, p_hat_sel);
    return r;
}

// Compute the shaded direct-lighting contribution given a pre-tested visibility.
// The caller shoots the shadow ray and passes `visible` (true = unoccluded).
float3 ReservoirShade(
    float3 N, float3 V,
    float3 baseColor, float metallic, float roughness,
    Reservoir r,
    bool visible)
{
    if (!visible || r.W <= 0.0f || r.M == 0u)
        return float3(0, 0, 0);

    float NdotL = saturate(dot(N, r.y_direction));
    if (NdotL <= 0.0f)
        return float3(0, 0, 0);

    float3 brdf = EvaluatePBR(baseColor, metallic, roughness, N, V, r.y_direction);
    return brdf * r.y_radiance * r.W;
}

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
    payload.radiance   = float3(0, 0, 0);
    payload.hit_t      = -1.0f;
    payload.missed     = 0u;
    payload.depth      = 0u;
    payload.sec_normal = float3(0, 1, 0);

    TraceRay(g_TLAS[g_Frame.tlas_slot],
             0u,             // RAY_FLAG_NONE
             0xFFu,          // instance mask
             0u,             // hit group index
             1u,             // contribution to hit group index
             0u,             // miss shader index
             ray,
             payload);

    g_OutputUAV[g_Frame.output_uav_slot][launchIdx] = float4(payload.radiance, 1.0f);

    // ---- Motion vectors and linear depth ------------------------------------
    if (g_Frame.motion_vector_uav_slot != 0xFFFFFFFF)
    {
        // Full motion vector (camera + object motion) in NDC space.
        // Written directly as prevNDC - currNDC so mvecScale = {1,1} (no extra scaling).
        // cameraMotionIncluded = eTrue tells DLSS-RR the MVs encode full camera motion.
        // Y is negated: D3D12 clip-space NDC has Y+ up, but DLSS-RR expects Y+ down
        // (screen-space convention where pixel row 0 is at the top).
        float2 motionVec = float2(0.0f, 0.0f);

        float3 mvWorldPos;
        if (payload.missed == 0u && payload.hit_t > 0.0f)
            mvWorldPos = ray.Origin + ray.Direction * payload.hit_t;
        else
            mvWorldPos = ray.Origin + ray.Direction * 1e5f; // sky: far-plane point

        {
            float4 currClip = mul(g_Frame.view_proj,      float4(mvWorldPos, 1.0f));
            float4 prevClip = mul(g_Frame.prev_view_proj, float4(mvWorldPos, 1.0f));

            float2 currNDC = currClip.xy / currClip.w;
            float2 prevNDC = prevClip.xy / prevClip.w;

            // DLSS-RR uses screen-space Y+ down; negate Y to convert from NDC Y+ up.
            float2 delta = prevNDC - currNDC;
            motionVec = float2(delta.x, -delta.y);
        }

        g_MotionVectorUAV[g_Frame.motion_vector_uav_slot][launchIdx] =
            float4(motionVec, 0.0f, 0.0f);
    }

    if (g_Frame.depth_uav_slot != 0xFFFFFFFF)
    {
        float d = 1.0f; // default: far plane
        if (payload.missed == 0u && payload.hit_t > 0.0f)
        {
            // Project the world-space hit point to get NDC depth (clip.z / clip.w).
            float3 worldPos = ray.Origin + ray.Direction * payload.hit_t;
            float4 clip     = mul(g_Frame.view_proj, float4(worldPos, 1.0f));
            d = saturate(clip.z / clip.w);
        }
        g_LinearDepthUAV[g_Frame.depth_uav_slot][launchIdx] = d;
    }
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
    payload.missed   = 1u;
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

    // V = outgoing direction toward the ray origin.
    // For primary rays (depth 0) this is the camera.
    // For secondary GI rays (depth 1) it is the direction back along the incoming ray.
    // Using camera_pos at depth 1 gives the wrong half-vector → EvaluatePBR returns 0
    // for any surface not directly facing the camera, making GI always black.
    float3 V = (payload.depth == 0u)
                   ? normalize(g_Frame.camera_pos - worldPos)
                   : normalize(-WorldRayDirection());
    if (dot(N, V) < 0.0f)
        N = -N;   // flip for double-sided surfaces

    uint2 pixelIdx = DispatchRaysIndex().xy;

    // ---- Write G-buffer AOV textures (DLSS-RR mandatory inputs) ----------------
    // Only write from primary-ray hits (depth 0). Secondary-ray hits must NOT
    // overwrite these — DispatchRaysIndex() is the same pixel for both depths,
    // so a depth-1 write would stomp the primary surface's G-buffer data.
    if (payload.depth == 0u)
    {
    if (g_Frame.albedo_uav_slot != 0xFFFFFFFF)
    {
        // Diffuse base color with metallic masking: metals have no diffuse albedo.
        float3 diffuseAlbedo = baseColor * (1.0f - metallic);
        g_AlbedoUAV[g_Frame.albedo_uav_slot][pixelIdx] = float4(diffuseAlbedo, 1.0f);
    }

    if (g_Frame.specular_albedo_uav_slot != 0xFFFFFFFF)
    {
        // Specular F0: dielectrics use 0.04, metals use baseColor.
        float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);
        g_SpecularAlbedoUAV[g_Frame.specular_albedo_uav_slot][pixelIdx] = float4(F0, 1.0f);
    }

    if (g_Frame.normals_uav_slot != 0xFFFFFFFF)
    {
        // Encode world-space shading normal into [0,1] range: n * 0.5 + 0.5
        float3 nEnc = N * 0.5f + 0.5f;
        g_NormalsUAV[g_Frame.normals_uav_slot][pixelIdx] = float4(nEnc, 1.0f);
    }

    if (g_Frame.roughness_uav_slot != 0xFFFFFFFF)
    {
        g_RoughnessUAV[g_Frame.roughness_uav_slot][pixelIdx] = roughness;
    }
    } // end depth == 0u guard for AOV writes

    // ---- ReSTIR DI — direct illumination via RIS + visibility ----------------
    uint rng = PcgSeed(pixelIdx, g_Frame.frame_index);

    DirectionalLight sun;
    sun.direction = normalize(g_Frame.sun_direction);
    sun.radiance  = g_Frame.sun_color * g_Frame.sun_intensity;

    Reservoir res = RIS_GenerateCandidates(baseColor, metallic, roughness, N, V, sun, rng);

    // Shadow ray for the selected reservoir sample — only at depth 0 (primary hits).
    // Secondary hits (depth 1) use unshadowed direct lighting to stay within recursion depth 2.
    bool visible = true;
    if (payload.depth == 0u)
    {
        ShadowPayload shadowPayload;
        shadowPayload.occluded = 1u;

        RayDesc shadowRay;
        shadowRay.Origin    = worldPos + N * 1e-3f;
        shadowRay.Direction = res.y_direction;
        shadowRay.TMin      = 1e-4f;
        shadowRay.TMax      = 1e6f;

        TraceRay(g_TLAS[g_Frame.tlas_slot],
                 0x4u | 0x8u, // RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
                 0xFFu,
                 0u, 1u,
                 1u,  // ShadowMiss index
                 shadowRay,
                 shadowPayload);

        visible = (shadowPayload.occluded == 0u);
    }
    float3 directLight = ReservoirShade(N, V, baseColor, metallic, roughness, res, visible);

    // ---- GI — single-bounce BRDF-importance-sampled Monte Carlo ----------------
    // For diffuse surfaces: cosine-hemisphere sampling (pdf = NdotL/PI).
    // For specular/metallic surfaces: GGX VNDF sampling so that mirror-like and
    // glossy reflections are captured correctly.
    // We select between the two lobes probabilistically (MIS one-sample model):
    //   p_spec = luminance of the Fresnel-weighted specular albedo / total albedo
    //   p_diff = 1 - p_spec
    // The chosen direction is traced and the estimator weight is BRDF / pdf.
    // DLSS-RR temporally accumulates the noisy 1-spp result.

    float3 giContrib = float3(0, 0, 0);

    if (g_Frame.gi_enabled != 0u && payload.depth == 0u)
    {
        // --- Decide which lobe to sample -----------------------------------
        float3 F0      = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);
        float  NdotV_g = saturate(dot(N, V));
        float3 F_view  = FresnelSchlick(NdotV_g, F0);
        float  lum_spec = dot(F_view, float3(0.2126f, 0.7152f, 0.0722f));
        float  lum_diff = (1.0f - metallic) * dot((1.0f - F_view) * baseColor,
                                                    float3(0.2126f, 0.7152f, 0.0722f));
        float  total    = lum_spec + lum_diff;
        float  p_spec   = (total > 1e-6f) ? saturate(lum_spec / total) : 0.0f;

        float  u0 = PcgRand(rng);
        float  u1 = PcgRand(rng);
        float  u2 = PcgRand(rng);

        float3 secDir;
        float  pdf;

        if (u0 < p_spec)
        {
            // --- Specular lobe: GGX VNDF importance sampling ---------------
            // Transform V into the tangent-space frame (T, B, N).
            float3 T = normalize(worldTangent);
            float3 B = normalize(worldBitangent);
            float3 V_local = float3(dot(V, T), dot(V, B), dot(V, N));

            float3 H_local = SampleGGX_VNDF(V_local, max(roughness * roughness, 0.01f), u1, u2);
            float3 H_world = normalize(H_local.x * T + H_local.y * B + H_local.z * N);

            secDir = reflect(-V, H_world);

            // GGX VNDF pdf for the sampled half-vector, converted to solid-angle pdf for L.
            // pdf_L = D_visible(H) / (4 * dot(V, H))
            float NdotH = saturate(dot(N, H_world));
            float VdotH = saturate(dot(V, H_world));
            float alpha = roughness * roughness;
            float alpha2 = alpha * alpha;
            float denom  = NdotH * NdotH * (alpha2 - 1.0f) + 1.0f;
            float D      = alpha2 / (PI * denom * denom);
            float NdotV_l = saturate(dot(N, V));
            float lambdaV = NdotV_l + sqrt(alpha2 + (1.0f - alpha2) * NdotV_l * NdotV_l);
            float G1V    = 2.0f * NdotV_l / max(lambdaV, 1e-6f);
            float D_vis  = D * G1V / max(2.0f * NdotV_l, 1e-6f);
            pdf = p_spec * D_vis / max(4.0f * VdotH, 1e-6f);
        }
        else
        {
            // --- Diffuse lobe: cosine-hemisphere sampling ------------------
            secDir = SampleCosineHemisphere(N, u1, u2);
            float NdotL_s = saturate(dot(N, secDir));
            pdf = (1.0f - p_spec) * CosineHemispherePdf(NdotL_s);
        }

        float NdotL_sec = saturate(dot(N, secDir));

        if (pdf > 1e-6f && NdotL_sec > 0.0f)
        {
            RayDesc secRay;
            secRay.Origin    = worldPos + N * 1e-3f;
            secRay.Direction = secDir;
            secRay.TMin      = 1e-4f;
            secRay.TMax      = 1e4f;

            PrimaryPayload secPayload;
            secPayload.radiance   = float3(0, 0, 0);
            secPayload.hit_t      = -1.0f;
            secPayload.missed     = 0u;
            secPayload.depth      = 1u;
            secPayload.sec_normal = float3(0, 1, 0);

            TraceRay(g_TLAS[g_Frame.tlas_slot],
                     0u, 0xFFu, 0u, 1u,
                     0u,
                     secRay, secPayload);

            float3 Lo      = secPayload.radiance;
            float3 brdfVal = EvaluatePBR(baseColor, metallic, roughness, N, V, secDir);
            // Estimator: BRDF(x,wi,wo) * Li / pdf  — EvaluatePBR includes NdotL
            giContrib = (brdfVal / pdf) * Lo;
        }
    }

    payload.radiance   = directLight + giContrib + emissive;
    payload.hit_t      = RayTCurrent();
    payload.missed     = 0u;
    payload.sec_normal = N;  // depth-1 callers use this as the secondary hit normal for GI reservoirs
}

