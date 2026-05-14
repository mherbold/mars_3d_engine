// =============================================================================
// denoiser.cpp
// MARS 3D Engine — DLSS 4 denoiser / upscaler / frame-generation implementation
// =============================================================================

#include "mars_engine/engine_api.h"
#include "mars_engine/renderer/denoiser.h"
#include "mars_engine/renderer/device_context.h"

#include <stdexcept>
#include <format>
#include <cmath>

#if MARS_HAS_STREAMLINE
#include <sl_helpers.h>
#pragma comment(lib, "sl.interposer.lib")
#endif

namespace mars
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
#if MARS_HAS_STREAMLINE
static sl::float4x4 to_sl(const Mat4x4& m)
{
    sl::float4x4 r{};
    // sl::float4x4 rows are sl::float4 {x,y,z,w} — index by column manually.
    for (int row = 0; row < 4; ++row)
    {
        float* dst = &r[row].x;
        dst[0] = m.m[row][0];
        dst[1] = m.m[row][1];
        dst[2] = m.m[row][2];
        dst[3] = m.m[row][3];
    }
    return r;
}

static sl::DLSSMode to_sl_mode(DlssMode m)
{
    switch (m)
    {
    case DlssMode::MaxPerformance:   return sl::DLSSMode::eMaxPerformance;
    case DlssMode::Balanced:         return sl::DLSSMode::eBalanced;
    case DlssMode::MaxQuality:       return sl::DLSSMode::eMaxQuality;
    case DlssMode::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
    case DlssMode::UltraQuality:     return sl::DLSSMode::eUltraQuality;
    case DlssMode::DLAA:             return sl::DLSSMode::eDLAA;
    default:                         return sl::DLSSMode::eOff;
    }
}
#endif

// ---------------------------------------------------------------------------
// pre_device_init — call BEFORE D3D12 device creation
// ---------------------------------------------------------------------------
void Denoiser::pre_device_init()
{
#if MARS_HAS_STREAMLINE
    sl::Preferences pref{};
    pref.showConsole      = false;
    pref.logLevel         = sl::LogLevel::eDefault;
    pref.pathsToPlugins   = nullptr;
    pref.numPathsToPlugins= 0;

    // Request the features we want Streamline to load.
    // sl.dlss_g requires sl.reflex as a hard dependency — it must be in this list.
    static const sl::Feature features[] =
    {
        sl::kFeatureDLSS,     // Super Resolution
        sl::kFeatureDLSS_RR,  // Ray Reconstruction
        sl::kFeatureDLSS_G,   // Multi Frame Generation
        sl::kFeatureReflex,   // Required by DLSS-G
    };
    pref.featuresToLoad    = features;
    pref.numFeaturesToLoad = 4;
    pref.flags             = sl::PreferenceFlags::eUseFrameBasedResourceTagging;

    // Engine identity — required for NGX initialization (DLSS-SR, DLSS-RR, DLSS-G).
    // NGX needs all three: engine type, version, and a project GUID.
    // The projectId can be any stable GUID string for development builds.
    pref.engine        = sl::EngineType::eCustom;
    pref.engineVersion = "1.0.0";
    pref.projectId     = "a1b2c3d4-e5f6-7890-abcd-ef1234567890";

    sl::Result res = slInit(pref, sl::kSDKVersion);
    if (res == sl::Result::eOk)
        MARS_LOG("[Denoiser] slInit() OK — Streamline v{}.{}.{}",
                     SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
    else
        MARS_LOG("[Denoiser] slInit() returned {} — DLSS will be unavailable.",
                     static_cast<int>(res));
#else
    MARS_LOG("[Denoiser] Streamline SDK not present — DLSS disabled.");
#endif
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
void Denoiser::init(DeviceContext& ctx,
                    uint32_t output_count,
                    DlssMode mode)
{
    if (m_initialised) return;

    m_output_count = output_count;
    m_mode         = mode;
    m_outputs.resize(output_count);

#if MARS_HAS_STREAMLINE
    // Wire the D3D12 device into Streamline now that it exists.
    sl::Result res = slSetD3DDevice(ctx.device());
    if (res != sl::Result::eOk)
    {
        MARS_LOG("[Denoiser] slSetD3DDevice failed ({}) — DLSS disabled.",
                     static_cast<int>(res));
        m_initialised = true;
        return;
    }

    // Check feature support against the selected adapter.
    LUID luid = ctx.adapter_luid();
    sl::AdapterInfo adapter_info{};
    adapter_info.deviceLUID      = reinterpret_cast<uint8_t*>(&luid);
    adapter_info.deviceLUIDSizeInBytes = sizeof(LUID);

    auto check = [&](sl::Feature feature) -> bool
    {
        sl::Result r = slIsFeatureSupported(feature, adapter_info);
        return r == sl::Result::eOk;
    };

    m_dlss_supported    = check(sl::kFeatureDLSS);
    m_dlss_rr_supported = check(sl::kFeatureDLSS_RR);
    m_dlss_g_supported  = check(sl::kFeatureDLSS_G);

    MARS_LOG("[Denoiser] Feature support — SR:{} RR:{} MFG:{}",
                 m_dlss_supported, m_dlss_rr_supported, m_dlss_g_supported);

    // Initialise per-output viewport handles.
    for (uint32_t i = 0; i < output_count; ++i)
    {
        m_outputs[i].viewport = sl::ViewportHandle{ i };
        m_outputs[i].frame_token = nullptr;
    }
#else
    (void)ctx;
#endif

    m_initialised = true;
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------
void Denoiser::shutdown()
{
    if (!m_initialised) return;

#if MARS_HAS_STREAMLINE
    // Free per-viewport resources.
    for (uint32_t i = 0; i < m_output_count; ++i)
    {
        if (m_dlss_rr_supported)
            slFreeResources(sl::kFeatureDLSS_RR, m_outputs[i].viewport);
        if (m_dlss_supported)
            slFreeResources(sl::kFeatureDLSS,    m_outputs[i].viewport);
        if (m_dlss_g_supported)
            slFreeResources(sl::kFeatureDLSS_G,  m_outputs[i].viewport);
    }
    slShutdown();
#endif

    m_outputs.clear();
    m_output_count = 0;
    m_initialised  = false;
}

// ---------------------------------------------------------------------------
// resize_output
// ---------------------------------------------------------------------------
void Denoiser::resize_output(uint32_t output_index,
                              uint32_t display_width, uint32_t display_height)
{
    if (output_index >= m_output_count) return;

    auto& out = m_outputs[output_index];
    out.display_width  = display_width;
    out.display_height = display_height;

#if MARS_HAS_STREAMLINE
    if (m_dlss_supported && m_mode != DlssMode::Off)
    {
        // Query the optimal internal render resolution.
        sl::DLSSOptions opts{};
        opts.mode         = to_sl_mode(m_mode);
        opts.outputWidth  = display_width;
        opts.outputHeight = display_height;

        sl::DLSSOptimalSettings settings{};
        if (slDLSSGetOptimalSettings(opts, settings) == sl::Result::eOk)
        {
            out.render_width  = settings.optimalRenderWidth;
            out.render_height = settings.optimalRenderHeight;
        }
        else
        {
            out.render_width  = display_width;
            out.render_height = display_height;
        }
    }
    else
    {
        out.render_width  = display_width;
        out.render_height = display_height;
    }
#else
    out.render_width  = display_width;
    out.render_height = display_height;
#endif

    m_dirty = true;

    MARS_LOG("[Denoiser] output[{}] display={}x{} render={}x{}",
                 output_index, display_width, display_height,
                 out.render_width, out.render_height);
}

// ---------------------------------------------------------------------------
// get_render_resolution
// ---------------------------------------------------------------------------
void Denoiser::get_render_resolution(uint32_t output_index,
                                      uint32_t& out_render_w,
                                      uint32_t& out_render_h) const
{
    if (output_index < m_outputs.size() &&
        m_outputs[output_index].render_width > 0)
    {
        out_render_w = m_outputs[output_index].render_width;
        out_render_h = m_outputs[output_index].render_height;
    }
    else
    {
        out_render_w = (output_index < m_outputs.size()) ? m_outputs[output_index].display_width  : 0;
        out_render_h = (output_index < m_outputs.size()) ? m_outputs[output_index].display_height : 0;
    }
}

// ---------------------------------------------------------------------------
// tag_resources
// ---------------------------------------------------------------------------
void Denoiser::tag_resources(void* cmd_buffer,
                              uint32_t output_index,
                              uint32_t frame_index,
                              ID3D12Resource* noisy_color,
                              ID3D12Resource* motion_vectors,
                              ID3D12Resource* depth,
                              ID3D12Resource* albedo,
                              ID3D12Resource* specular_albedo,
                              ID3D12Resource* normals,
                              ID3D12Resource* roughness,
                              ID3D12Resource* output_color,
                              uint32_t render_width,
                              uint32_t render_height,
                              uint32_t display_width,
                              uint32_t display_height)
{
#if MARS_HAS_STREAMLINE
    if (!m_initialised || output_index >= m_output_count) return;

    auto& out = m_outputs[output_index];

    out.constants_set = false;

    // Obtain (or refresh) a frame token.
    sl::FrameToken* token = nullptr;
    slGetNewFrameToken(token, &frame_index);
    out.frame_token = token;
    if (!token) return;

    sl::Extent render_ext{ 0, 0, render_width, render_height };
    sl::Extent display_ext{ 0, 0, display_width, display_height };

    // Build tag list.
    // The renderer transitions noisy_color, motion_vectors, and depth from
    // D3D12_RESOURCE_STATE_UNORDERED_ACCESS → COMMON before calling tag_resources,
    // so all resources are in COMMON here. Streamline's internal legacy
    // ResourceBarrier transitions must match; passing COMMON avoids
    // RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH (#527).
    //
    // NOTE: sl::ResourceTag holds a raw pointer (tag.resource = &r). The sl::Resource
    // objects must remain alive for the entire duration of slSetTagForFrame. They are
    // stored in `resources` which has the same lifetime as `tags`.
    std::vector<sl::Resource>    resources;
    std::vector<sl::ResourceTag> tags;
    // Reserve up front so push_back never reallocates and invalidates the pointers
    // that tag.resource already holds into the vector.
    resources.reserve(8);
    tags.reserve(8);
    auto add = [&](sl::BufferType type, ID3D12Resource* res, const sl::Extent& ext,
                   sl::ResourceLifecycle lc    = sl::ResourceLifecycle::eValidUntilEvaluate,
                   D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON)
    {
        if (!res) return;
        sl::Resource r;
        r.type    = sl::ResourceType::eTex2d;
        r.native  = res;
        r.state   = state;
        resources.push_back(r);
        sl::ResourceTag tag;
        tag.resource  = &resources.back();
        tag.type      = type;
        tag.lifecycle = lc;
        tag.extent    = ext;
        tags.push_back(tag);
    };

    // noisy_color, motion_vectors, and depth are transitioned to COMMON by the
    // renderer before tag_resources is called, so all states here are COMMON.
    //
    // IMPORTANT: do NOT alias noisy_color as kBufferTypeAlbedo / kBufferTypeSpecularAlbedo
    // / kBufferTypeNormals / kBufferTypeRoughness. Tagging the same ID3D12Resource
    // under multiple buffer types causes Streamline to emit a ResourceBarrier for
    // each tag on the same subresource, producing
    // RESOURCE_BARRIER_DUPLICATE_SUBRESOURCE_TRANSITIONS warnings AND corrupting
    // Streamline's internal state machine for sl.dlss_d.mvec, which then issues a
    // barrier with Before=COMMON when the actual tracked state is
    // PIXEL_SHADER_RESOURCE → RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH #527 →
    // D3D12 debug break → 0x87A crash at ExecuteCommandLists.
    //
    // Only tag kBufferTypeNormals / kBufferTypeRoughness when dedicated G-buffer
    // resources are actually provided. Once dedicated AOV textures are wired up
    // (see PLAN.md §6 "Albedo AOV / Normals AOV / Roughness AOV TODO"), pass them
    // here instead.
    add(sl::kBufferTypeScalingInputColor,  noisy_color,     render_ext);
    add(sl::kBufferTypeMotionVectors,      motion_vectors,  render_ext);
    add(sl::kBufferTypeDepth,              depth,           render_ext);
    // Dedicated AOV textures \u2014 each resource is distinct so there is no aliasing.
    // albedo and specular_albedo are mandatory for DLSS-RR; normals and roughness optional.
    add(sl::kBufferTypeAlbedo,             albedo,          render_ext);
    add(sl::kBufferTypeSpecularAlbedo,     specular_albedo, render_ext);
    if (normals)   add(sl::kBufferTypeNormals,   normals,   render_ext);
    if (roughness) add(sl::kBufferTypeRoughness, roughness, render_ext);
    // DenoisedOutputUAV is created in COMMON state and is returned to COMMON via an
    // Enhanced Barrier at the end of each frame (after slEvaluateFeature). Tagging it
    // as COMMON lets Streamline issue a valid legacy ResourceBarrier on every frame.
    add(sl::kBufferTypeScalingOutputColor, output_color,   display_ext,
        sl::ResourceLifecycle::eValidUntilPresent,
        D3D12_RESOURCE_STATE_COMMON);
    // kBufferTypeHUDLessColor intentionally omitted: tagging the same resource as
    // both ScalingOutputColor and HUDLessColor generates duplicate subresource
    // barrier warnings (RESOURCE_BARRIER_DUPLICATE_SUBRESOURCE_TRANSITIONS) and
    // contributes to the barrier-type conflict on the denoised output texture.
    // A separate HUD-less buffer should be tagged here once one is allocated.

    slSetTagForFrame(*token, out.viewport,
                     tags.data(), static_cast<uint32_t>(tags.size()),
                     static_cast<sl::CommandBuffer*>(cmd_buffer));
#else
    (void)cmd_buffer; (void)output_index; (void)frame_index;
    (void)noisy_color; (void)motion_vectors; (void)depth;
    (void)albedo; (void)specular_albedo;
    (void)normals; (void)roughness; (void)output_color;
    (void)render_width; (void)render_height;
    (void)display_width; (void)display_height;
#endif
}

// ---------------------------------------------------------------------------
// evaluate_dlss_rr (Ray Reconstruction)
// ---------------------------------------------------------------------------
bool Denoiser::evaluate_dlss_rr(void* cmd_buffer,
                                 uint32_t output_index,
                                 uint32_t frame_index,
                                 const Mat4x4& view,
                                 const Mat4x4& view_inv,
                                 const Mat4x4& proj,
                                 const Mat4x4& prev_view_inv,
                                 const Mat4x4& prev_proj_inv,
                                 float jitter_x, float jitter_y,
                                 uint32_t render_width,
                                 uint32_t render_height,
                                 uint32_t display_width,
                                 uint32_t display_height)
{
#if MARS_HAS_STREAMLINE
    if (!m_initialised || !m_dlss_rr_supported) return false;
    (void)frame_index;
    (void)prev_view_inv;
    (void)prev_proj_inv;
    (void)render_width;
    (void)render_height;

    auto& out = m_outputs[output_index];
    if (!out.frame_token) return false;

    // Configure DLSS-RR options (worldToCameraView = view matrix, its inverse = view_inv).
    sl::DLSSDOptions opts{};
    opts.mode              = to_sl_mode(m_mode);
    opts.outputWidth       = display_width;
    opts.outputHeight      = display_height;
    opts.colorBuffersHDR   = sl::Boolean::eTrue;
    opts.worldToCameraView = to_sl(view);
    opts.cameraViewToWorld = to_sl(view_inv);
    slDLSSDSetOptions(out.viewport, opts);

    // Per-frame common constants.
    // view_inv is the camera-to-world matrix (row-major):
    //   row 0 = right  vector (x,y,z)
    //   row 1 = up     vector (x,y,z)
    //   row 2 = fwd    vector (x,y,z)
    //   row 3 = camera position (x,y,z)
    // proj is the standard D3D12 perspective matrix:
    //   m[0][0] = 1/(aspect*tan(vFOV/2)),  m[1][1] = 1/tan(vFOV/2)
    //
    // cameraMotionIncluded = eTrue: motion vectors already encode full camera + object
    // motion, so DLSS-RR uses them directly for reprojection. In this mode we must pass
    // identity for clipToPrevClip/prevClipToClip — passing real transforms would cause
    // DLSS-RR to apply camera reprojection a second time on top of the MVs, moving
    // everything at 2x the correct speed.
    Mat4x4 proj_inv  = proj.inverse();
    Mat4x4 identity  = Mat4x4::identity();

    sl::Constants constants{};
    constants.cameraViewToClip = to_sl(proj);
    constants.clipToCameraView = to_sl(proj_inv);
    constants.clipToPrevClip   = to_sl(identity);
    constants.prevClipToClip   = to_sl(identity);

    // Camera position and orientation derived from view_inv (camera-to-world).
    constants.cameraPos   = { view_inv.m[3][0], view_inv.m[3][1], view_inv.m[3][2] };
    constants.cameraRight = { view_inv.m[0][0], view_inv.m[0][1], view_inv.m[0][2] };
    constants.cameraUp    = { view_inv.m[1][0], view_inv.m[1][1], view_inv.m[1][2] };
    constants.cameraFwd   = { view_inv.m[2][0], view_inv.m[2][1], view_inv.m[2][2] };

    // Vertical FOV and aspect ratio derived from the projection matrix.
    // proj.m[1][1] = 1/tan(vFOV/2),  proj.m[0][0] = 1/(aspect*tan(vFOV/2))
    constants.cameraFOV         = 2.0f * std::atan(1.0f / proj.m[1][1]);
    constants.cameraAspectRatio = proj.m[1][1] / proj.m[0][0];

    // Pinhole offset — zero for a standard centred projection.
    constants.cameraPinholeOffset = sl::float2{ 0.0f, 0.0f };

    // Sentinel value written into the motion-vector buffer for pixels with no
    // valid motion (e.g. sky).  1e8 is the conventional large-but-finite value.
    constants.motionVectorsInvalidValue = 1e8f;

    constants.jitterOffset       = sl::float2{ jitter_x, jitter_y };
    // Motion vectors are written as (prevNDC - currNDC) where NDC spans [-1,+1] (2 units).
    // DLSS-RR expects values in normalized screen space where 1.0 = full screen width.
    // NDC spans 2 units for a full-screen displacement, so scale by 0.5 to convert.
    constants.mvecScale = sl::float2{ 0.5f, 0.5f };
    constants.depthInverted        = sl::Boolean::eFalse;
    constants.cameraNear           = 0.1f;
    constants.cameraFar            = 100000.0f;
    // Motion vectors written by the path tracer include full camera + object motion
    // (pixel-space displacement from previous to current frame for each hit point).
    // eTrue tells DLSS-RR to use them directly for reprojection.
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D      = sl::Boolean::eFalse;
    constants.reset                = sl::Boolean::eFalse;
    slSetConstants(constants, *out.frame_token, out.viewport);
    out.constants_set = true;

    const sl::BaseStructure* inputs[] = { &out.viewport, &opts };
    sl::Result result = slEvaluateFeature(sl::kFeatureDLSS_RR, *out.frame_token, inputs, 2,
                                          static_cast<sl::CommandBuffer*>(cmd_buffer));
    return result == sl::Result::eOk;
#else
    (void)cmd_buffer; (void)output_index; (void)frame_index;
    (void)view; (void)view_inv; (void)proj;
    (void)prev_view_inv; (void)prev_proj_inv;
    (void)jitter_x; (void)jitter_y;
    (void)render_width; (void)render_height;
    (void)display_width; (void)display_height;
    return false;
#endif
}

// ---------------------------------------------------------------------------
// evaluate_dlss_sr (Super Resolution)
// ---------------------------------------------------------------------------
void Denoiser::evaluate_dlss_sr(void* cmd_buffer,
                                 uint32_t output_index,
                                 uint32_t frame_index,
                                 float jitter_x, float jitter_y,
                                 uint32_t render_width,
                                 uint32_t render_height,
                                 uint32_t display_width,
                                 uint32_t display_height)
{
#if MARS_HAS_STREAMLINE
    if (!m_initialised || !m_dlss_supported) return;
    (void)frame_index;
    if (output_index >= m_output_count) return;

    auto& out = m_outputs[output_index];
    if (!out.frame_token) return;

    sl::DLSSOptions opts{};
    opts.mode         = to_sl_mode(m_mode);
    opts.outputWidth  = display_width;
    opts.outputHeight = display_height;
    opts.colorBuffersHDR = sl::Boolean::eTrue;
    slDLSSSetOptions(out.viewport, opts);

    // slSetConstants is shared per frame/viewport; skip it if evaluate_dlss_rr
    // already called it for this frame to avoid the Streamline error.
    if (!out.constants_set)
    {
        sl::Constants constants{};
        constants.jitterOffset       = sl::float2{ jitter_x, jitter_y };
        constants.mvecScale          = sl::float2{
            1.0f / static_cast<float>(render_width),
            1.0f / static_cast<float>(render_height)
        };
        constants.depthInverted        = sl::Boolean::eFalse;
        constants.cameraNear           = 0.1f;
        constants.cameraFar            = 100000.0f;
        constants.cameraMotionIncluded = sl::Boolean::eFalse;
        constants.motionVectors3D      = sl::Boolean::eFalse;
        constants.reset                = sl::Boolean::eFalse;
        slSetConstants(constants, *out.frame_token, out.viewport);
        out.constants_set = true;
    }

    const sl::BaseStructure* inputs[] = { &out.viewport, &opts };
    slEvaluateFeature(sl::kFeatureDLSS, *out.frame_token, inputs, 2,
                      static_cast<sl::CommandBuffer*>(cmd_buffer));
#else
    (void)cmd_buffer; (void)output_index; (void)frame_index;
    (void)jitter_x; (void)jitter_y;
    (void)render_width; (void)render_height;
    (void)display_width; (void)display_height;
#endif
}

// ---------------------------------------------------------------------------
// evaluate_dlss_g (Multi Frame Generation)
// ---------------------------------------------------------------------------
void Denoiser::evaluate_dlss_g(void* cmd_buffer,
                                uint32_t output_index,
                                uint32_t frame_index)
{
#if MARS_HAS_STREAMLINE
    if (!m_initialised || !m_dlss_g_supported) return;
    (void)cmd_buffer;
    (void)frame_index;
    if (output_index >= m_output_count) return;

    auto& out = m_outputs[output_index];
    if (!out.frame_token) return;

    // DLSS-G (Multi Frame Generation) is an *implicit* Streamline feature.
    // It operates by hooking IDXGISwapChain::Present via the Streamline interposer
    // and automatically generates the synthetic frames at Present time.
    // It deliberately does NOT register evaluateFeature callbacks — calling
    // slEvaluateFeature(kFeatureDLSS_G) always returns
    // "Could not find 'evaluateFeature' callbacks for feature 1000" and causes
    // Streamline's internal sl.dlss_d.mvec buffer to be left in
    // D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE after the failed evaluate.
    // On the next frame, DLSS-RR tags that buffer as COMMON and tries to barrier
    // from COMMON, producing RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH (#527).
    //
    // The correct usage: call slDLSSGSetOptions() once per frame before Present
    // so the interposer knows the desired mode.  Present triggers MFG implicitly.
    sl::DLSSGOptions opts{};
    opts.mode                = sl::DLSSGMode::eOn;
    opts.numFramesToGenerate = 1;  // RTX 40 series: 1 generated frame = 2× total
    slDLSSGSetOptions(out.viewport, opts);
    // Do NOT call slEvaluateFeature for kFeatureDLSS_G — see comment above.
#else
    (void)cmd_buffer; (void)output_index; (void)frame_index;
#endif
}

} // namespace mars
