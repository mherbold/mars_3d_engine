// =============================================================================
// renderer.h
// MARS 3D Engine — Renderer public API
//
// The Renderer owns a DeviceContext and a DisplayOutput.  For M1 it clears
// the back buffer to a solid color each frame and presents.  Later milestones
// will add the DXR path-tracing pipeline, denoising, and DLSS integration.
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../d3d12_agility.h"
#include "device_context.h"
#include "display_output.h"

#include <wrl/client.h>
#include <array>
#include <cstdint>

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

    // Create device, queues, swap chain, and per-frame command allocators.
    void init(HWND hwnd, uint32_t width, uint32_t height);
    void shutdown();

    // Call when the window is resized.
    void on_resize(uint32_t width, uint32_t height);

    // Render one frame: clear the back buffer and present.
    void render_frame();

    // ---- Accessors ----------------------------------------------------------
    DeviceContext&  device_context() { return m_device_ctx; }
    DisplayOutput&  display_output() { return m_display_output; }

private:
    void create_frame_resources();
    void release_frame_resources();
    void wait_for_frame(uint32_t frame_index);

    DeviceContext   m_device_ctx;
    DisplayOutput   m_display_output;

    // Per-frame command allocators (one per back buffer).
    std::array<ComPtr<ID3D12CommandAllocator>, k_frame_count>  m_cmd_allocators;
    ComPtr<ID3D12GraphicsCommandList6>                         m_cmd_list;

    // Per-frame fence values used to track GPU progress.
    std::array<uint64_t, k_frame_count>  m_frame_fence_values{};
    ComPtr<ID3D12Fence>                  m_frame_fence;
    HANDLE                               m_frame_fence_event = nullptr;
    uint64_t                             m_frame_fence_next  = 0;

    bool m_initialised = false;
};

} // namespace mars
