// =============================================================================
// renderer.h
// MARS 3D Engine — Renderer public API
//
// The Renderer owns a DeviceContext and a DisplayManager.  For M1/M2 it
// clears all active back buffers to a solid color each frame and presents.
// M4 adds the DXR path-tracing pipeline via PathTracer.
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../d3d12_agility.h"
#include "../math/math_types.h"
#include "device_context.h"
#include "display_manager.h"
#include "path_tracer.h"
#include "denoiser.h"
#include "../asset/resource_manager.h"
#include "../scene/scene.h"
#include "../animation/animation_system.h"

#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <vector>
#include <string>

namespace mars
{

using Microsoft::WRL::ComPtr;

// =============================================================================
// Renderer
// =============================================================================
class MARS_ENGINE_API Renderer
{
public:
    Renderer()  = default;
    ~Renderer() { shutdown(); }

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Single-monitor convenience init (M1 API preserved for test_app).
    // Creates one DisplayOutput for `hwnd` at the given resolution.
    void init(HWND hwnd, uint32_t width, uint32_t height);

    // Multi-monitor init.  Reads `display_json_path` to build display configs;
    // `hwnds` provides one HWND per configured monitor (or one shared HWND).
    void init(const std::string&          display_json_path,
              const std::vector<HWND>&    hwnds);

    void shutdown();

    // Call when output `output_index` has been resized.
    void on_resize(uint32_t output_index, uint32_t width, uint32_t height);

    // Single-output resize convenience (maps to output 0).
    void on_resize(uint32_t width, uint32_t height);

    // Render one frame across all active DisplayOutputs.
    // If the PathTracer has been initialised and a TLAS is present it will
    // trace rays; otherwise falls back to the solid-color clear.
    void render_frame();

    // Advance simulation by `delta_time` seconds.
    // Updates all active animation states and, for skinned meshes,
    // uploads bone palettes and triggers GPU skinning + BLAS refit.
    // Call this once per frame before render_frame().
    void update(float delta_time);

    // ---- Camera / scene wiring ------------------------------------------

    // Set the camera transform for the given output index.
    // view_inv and proj_inv are inverse view and projection matrices.
    void set_camera(uint32_t output_index,
                    const Vec3& position,
                    const Mat4x4& view_inv,
                    const Mat4x4& proj_inv);

    // Load a .marsscene file and build the DXR acceleration structure.
    // Must be called after init(). Returns false on parse/load errors.
    bool load_scene(const std::string& marsscene_path);

    // Set the animated wind description used for cloth simulation.
    // Call after load_scene() to override the scene-file wind, or let
    // load_scene() populate it automatically.
    void set_wind_desc(const WindDesc& desc) { m_wind_desc = desc; m_wind_state = {}; }
    const WindDesc& wind_desc() const        { return m_wind_desc; }

    // Returns the instantaneous evaluated wind vector (updated each update() call).
    Vec3 wind() const { return m_wind_current; }

    // Rebuild the TLAS from the current scene (e.g. after adding instances).
    void rebuild_tlas();

    // ---- PathTracer access -----------------------------------------------

    PathTracer&       path_tracer()       { return m_path_tracer; }
    const PathTracer& path_tracer() const { return m_path_tracer; }

    // ---- Denoiser (DLSS 4) access ----------------------------------------

    Denoiser&         denoiser()          { return m_denoiser; }
    const Denoiser&   denoiser()    const { return m_denoiser; }

    // ---- Scene / resource access -----------------------------------------
    Scene&            scene()             { return m_scene; }
    const Scene&      scene()       const { return m_scene; }
    ResourceManager&  resource_manager()  { return m_resource_mgr; }
    AnimationSystem&  animation_system()  { return m_anim_system; }

    // ---- Accessors ----------------------------------------------------------
    DeviceContext&   device_context()              { return m_device_ctx; }
    DisplayManager&  display_manager()             { return m_display_manager; }

    // Convenience: return the primary (index 0) DisplayOutput.
    DisplayOutput&   primary_display()             { return m_display_manager.output(0); }

private:
    void init_internal(const std::vector<DisplayConfig>& configs,
                       const std::vector<HWND>&          hwnds);
    void create_frame_resources();
    void release_frame_resources();
    void wait_for_frame(uint32_t frame_index);

    // Render using DXR path tracing (when PathTracer is ready).
    void render_frame_path_traced();
    // Fallback: solid-color clear (M1/M2 behaviour).
    void render_frame_clear();

    DeviceContext   m_device_ctx;
    DisplayManager  m_display_manager;
    PathTracer      m_path_tracer;
    Denoiser        m_denoiser;
    ResourceManager m_resource_mgr;
    Scene           m_scene;
    AnimationSystem m_anim_system;

    // Per-frame command allocators (one per back buffer).
    std::array<ComPtr<ID3D12CommandAllocator>, k_frame_count>  m_cmd_allocators;
    ComPtr<ID3D12GraphicsCommandList6>                         m_cmd_list;

    // Per-frame fence values used to track GPU progress.
    std::array<uint64_t, k_frame_count>  m_frame_fence_values{};
    ComPtr<ID3D12Fence>                  m_frame_fence;
    HANDLE                               m_frame_fence_event = nullptr;
    uint64_t                             m_frame_fence_next  = 0;

    // Per-output camera state (updated via set_camera()).
    struct CameraState
    {
        Vec3    position     = {};
        Mat4x4  view_inv     = Mat4x4::identity();
        Mat4x4  proj_inv     = Mat4x4::identity();
        // Previous-frame matrices (for motion vector / DLSS reprojection).
        Mat4x4  prev_view_proj = Mat4x4::identity();
        Mat4x4  prev_view_inv  = Mat4x4::identity();
        Mat4x4  prev_proj_inv  = Mat4x4::identity();
    };
    std::vector<CameraState> m_cameras;

    uint32_t m_frame_index       = 0;
    bool     m_initialised       = false;
    bool     m_rigid_nodes_dirty = false;
    bool     m_cloth_dirty       = false;   // true when cloth instances need GPU sim + BLAS refit
    float    m_last_delta_time        = 0.0f;  // stored by update(), used by render_frame_path_traced()
    float    m_cloth_time_accumulator  = 0.0f;  // leftover time carried between frames for fixed-step cloth sim

    // ---- Wind state ---------------------------------------------------------
    WindDesc m_wind_desc    = {};        // scene-file parameters (loaded once)
    Vec3     m_wind_current = {};        // evaluated wind vector this frame (IIR-smoothed output)

    // Runtime phase accumulators for the three oscillator layers.
    struct WindState
    {
        float gust_phase      = 0.0f;
        float wander_phase    = 0.0f;
        float micro_phase_x   = 0.0f;
        float micro_phase_z   = 0.0f;
    } m_wind_state;

    // Per-output flag: true when slEvaluateFeature (DLSS-RR) left DenoisedOutputUAV
    // in legacy D3D12_RESOURCE_STATE_UNORDERED_ACCESS (Streamline always issues a
    // COMMON→UAV barrier before writing, regardless of whether slEvaluateFeature
    // ultimately succeeds). Must be cleaned up with a UAV→COMMON barrier on the
    // following frame if slEvaluateFeature failed (rr_evaluated==false).
    std::vector<bool> m_denoised_in_uav_state;
};

} // namespace mars
