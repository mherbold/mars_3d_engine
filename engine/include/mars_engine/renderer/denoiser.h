// =============================================================================
// denoiser.h
// MARS 3D Engine — DLSS 4 denoiser / upscaler / frame-generation integration
//
// Wraps the NVIDIA Streamline SDK v2.x to provide:
//   • DLSS 4 Ray Reconstruction  (sl.dlss_d / DLSSDOptions)  — primary denoiser
//   • DLSS 4 Super Resolution    (sl.dlss   / DLSSOptions)   — upscaler
//   • DLSS 4 Multi Frame Gen     (sl.dlss_g / DLSSGOptions)  — frame generation
//
// Integration notes:
//   - slInit() must be called *before* the D3D12 device and DXGI factory are
//     created so the Streamline interposer can wrap them.  DeviceContext calls
//     Denoiser::pre_device_init() as the very first thing.
//   - slSetD3DDevice() is called once the device exists.
//   - One sl::ViewportHandle per DisplayOutput (keyed by output_index).
//   - tag_resources() must be called each frame *before* evaluate_*().
//   - evaluate_dlss_rr() replaces the tone-map blit for denoised outputs.
//   - evaluate_dlss_sr() upscales the RR output to native res.
//   - evaluate_dlss_g() runs immediately before Present.
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../d3d12_agility.h"
#include "../math/math_types.h"

#include <wrl/client.h>
#include <cstdint>
#include <vector>

// Streamline headers — included conditionally so the engine compiles even
// when the SDK is absent (MARS::Streamline target will be missing in that case).
#if __has_include(<sl.h>)
#  include <sl.h>
#  include <sl_dlss.h>
#  include <sl_dlss_d.h>
#  include <sl_dlss_g.h>
#  include <sl_consts.h>
#  define MARS_HAS_STREAMLINE 1
#else
#  define MARS_HAS_STREAMLINE 0
#endif

namespace mars
{

using Microsoft::WRL::ComPtr;
class DeviceContext;

// =============================================================================
// DlssMode — user-facing quality preset (mirrors sl::DLSSMode)
// =============================================================================
enum class DlssMode : uint32_t
{
    Off             = 0,  // No upscaling — render at native resolution
    MaxPerformance  = 1,  // ~50 % render res
    Balanced        = 2,  // ~58 % render res
    MaxQuality      = 3,  // ~67 % render res
    UltraPerformance= 4,  // ~33 % render res
    UltraQuality    = 5,  // ~77 % render res
    DLAA            = 6,  // 100 % render res (anti-aliasing only)
};

// =============================================================================
// Denoiser — owns all per-frame DLSS interactions
// =============================================================================
class MARS_ENGINE_API Denoiser
{
public:
    Denoiser()  = default;
    ~Denoiser() { shutdown(); }

    Denoiser(const Denoiser&)            = delete;
    Denoiser& operator=(const Denoiser&) = delete;

    // -------------------------------------------------------------------------
    // Must be called BEFORE the D3D12 device is created.
    // Calls slInit() so the Streamline interposer can wrap DXGI/D3D12.
    // Safe to call even when the Streamline SDK is absent (no-op).
    // -------------------------------------------------------------------------
    static void pre_device_init();

    // -------------------------------------------------------------------------
    // Called once the D3D12 device exists.
    // -------------------------------------------------------------------------
    void init(DeviceContext& ctx,
              uint32_t output_count,
              DlssMode mode = DlssMode::MaxQuality);

    void shutdown();

    // -------------------------------------------------------------------------
    // Resize / reconfigure a viewport when output dimensions change.
    // -------------------------------------------------------------------------
    void resize_output(uint32_t output_index,
                       uint32_t display_width, uint32_t display_height);

    // Returns the render (internal) resolution for the given display output
    // after DLSS SR is applied.  When DLSS is Off the render res equals the
    // display res.
    void get_render_resolution(uint32_t output_index,
                               uint32_t& out_render_w,
                               uint32_t& out_render_h) const;

    // -------------------------------------------------------------------------
    // Per-frame resource tagging (must precede evaluate_*)
    // cmd_buffer is the ID3D12GraphicsCommandList* cast to void*.
    // -------------------------------------------------------------------------
    void tag_resources(void* cmd_buffer,
                       uint32_t output_index,
                       uint32_t frame_index,
                       ID3D12Resource* noisy_color,       // path-tracer RGBA16F UAV
                       ID3D12Resource* motion_vectors,    // R16G16 motion vector UAV
                       ID3D12Resource* depth,             // R32F linear depth UAV
                       ID3D12Resource* albedo,            // RGBA16F albedo AOV UAV (required by DLSS-RR)
                       ID3D12Resource* specular_albedo,   // RGBA16F specular albedo AOV UAV (required by DLSS-RR)
                       ID3D12Resource* normals,           // RGBA16F world normals AOV UAV (optional)
                       ID3D12Resource* roughness,         // RGBA16F roughness AOV UAV (optional)
                       ID3D12Resource* output_color,      // denoised / upscaled output UAV
                       uint32_t render_width,
                       uint32_t render_height,
                       uint32_t display_width,
                       uint32_t display_height);

    // -------------------------------------------------------------------------
    // Evaluate DLSS Ray Reconstruction (denoiser).
    // Runs the DLSSD inference pass; output_color receives the denoised result.
    // Returns true if slEvaluateFeature was called (i.e. the denoised output
    // texture was written and its layout was changed by Streamline), false if
    // the call was skipped (unsupported, missing token, etc.).
    // The caller MUST issue the post-evaluate Enhanced Barrier only when this
    // returns true.
    // -------------------------------------------------------------------------
    bool evaluate_dlss_rr(void* cmd_buffer,
                          uint32_t output_index,
                          uint32_t frame_index,
                          const Mat4x4& view,
                          const Mat4x4& view_inv,
                          const Mat4x4& proj,
                          float jitter_x, float jitter_y,
                          uint32_t render_width,
                          uint32_t render_height,
                          uint32_t display_width,
                          uint32_t display_height);

    // -------------------------------------------------------------------------
    // Evaluate DLSS Super Resolution (upscaler).
    // Reads from the denoised render-res texture; writes to display-res output.
    // -------------------------------------------------------------------------
    void evaluate_dlss_sr(void* cmd_buffer,
                          uint32_t output_index,
                          uint32_t frame_index,
                          float jitter_x, float jitter_y,
                          uint32_t render_width,
                          uint32_t render_height,
                          uint32_t display_width,
                          uint32_t display_height);

    // -------------------------------------------------------------------------
    // Evaluate DLSS-G Multi Frame Generation.
    // Must be called immediately before Present.
    // -------------------------------------------------------------------------
    void evaluate_dlss_g(void* cmd_buffer,
                         uint32_t output_index,
                         uint32_t frame_index);

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------
    bool       is_initialised()      const { return m_initialised; }
    bool       dlss_supported()      const { return m_dlss_supported; }
    bool       dlss_rr_supported()   const { return m_dlss_rr_supported; }
    bool       dlss_g_supported()    const { return m_dlss_g_supported; }
    DlssMode   mode()                const { return m_mode; }
    void       set_mode(DlssMode m)        { m_mode = m; m_dirty = true; }

private:
    struct PerOutput
    {
        uint32_t display_width  = 0;
        uint32_t display_height = 0;
        uint32_t render_width   = 0;
        uint32_t render_height  = 0;
#if MARS_HAS_STREAMLINE
        sl::ViewportHandle viewport{ 0 };
        sl::FrameToken*    frame_token = nullptr;
        bool               constants_set = false;
#endif
    };

    void reconfigure_viewports(DeviceContext& ctx);

    bool       m_initialised      = false;
    bool       m_dlss_supported   = false;
    bool       m_dlss_rr_supported= false;
    bool       m_dlss_g_supported = false;
    bool       m_dirty            = true;
    DlssMode   m_mode             = DlssMode::MaxQuality;
    uint32_t   m_output_count     = 0;

    std::vector<PerOutput> m_outputs;
};

} // namespace mars
