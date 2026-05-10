// =============================================================================
// display_output.h
// MARS 3D Engine — Display Output (DXGI swap chain + HDR color space)
//
// Each monitor driven by the engine gets one DisplayOutput instance.
// Responsibilities:
//   - Create and manage a DXGI swap chain.
//   - Detect HDR support and set the appropriate color space
//     (HDR10/PQ, scRGB, or SDR ACES tone-map fallback).
//   - Expose back-buffer render target views.
//   - Handle window resize / alt-enter full-screen transitions.
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../d3d12_agility.h"
#include "device_context.h"

#include <wrl/client.h>
#include <cstdint>

namespace mars
{

using Microsoft::WRL::ComPtr;

// =============================================================================
// HdrMode — chosen color space for this output
// =============================================================================
enum class HdrMode : uint8_t
{
    SDR   = 0,  // sRGB / ACES SDR tone map
    HDR10 = 1,  // HDR10 / ST.2084 PQ (10-bit UNORM back buffer)
    scRGB = 2,  // scRGB linear (16-bit FLOAT back buffer, Windows HDR)
};

// =============================================================================
// DisplayOutput
// =============================================================================
class MARS_ENGINE_API DisplayOutput
{
public:
    DisplayOutput()  = default;
    ~DisplayOutput() { shutdown(); }

    DisplayOutput(const DisplayOutput&)            = delete;
    DisplayOutput& operator=(const DisplayOutput&) = delete;

    // Create the swap chain for `hwnd`.
    // `device_ctx` must outlive this object.
    void init(DeviceContext& device_ctx, HWND hwnd, uint32_t width, uint32_t height);
    void shutdown();

    // Call after the window has been resized.
    void resize(uint32_t width, uint32_t height);

    // ---- Per-frame helpers ---------------------------------------------------

    // Returns the back-buffer index that should be rendered into this frame.
    uint32_t current_back_buffer_index() const;

    // The back-buffer resource for the current frame.
    ID3D12Resource*  current_back_buffer() const;

    // CPU descriptor handle for the current back-buffer RTV.
    D3D12_CPU_DESCRIPTOR_HANDLE current_rtv() const;

    // Record a resource-barrier transition on `cmd_list` for the back buffer.
    void transition(ID3D12GraphicsCommandList* cmd_list,
                    D3D12_RESOURCE_STATES from,
                    D3D12_RESOURCE_STATES to) const;

    // Present the rendered frame.
    void present(bool vsync = true);

    // ---- Accessors ----------------------------------------------------------
    HdrMode     hdr_mode()           const { return m_hdr_mode; }
    DXGI_FORMAT back_buffer_format() const { return m_back_buffer_format; }
    uint32_t    width()              const { return m_width; }
    uint32_t    height()             const { return m_height; }
    IDXGISwapChain4* swap_chain()    const { return m_swap_chain.Get(); }

private:
    void create_swap_chain(HWND hwnd);
    void detect_hdr_support();
    void create_rtvs();
    void release_rtvs();

    DeviceContext*               m_device_ctx    = nullptr;
    ComPtr<IDXGISwapChain4>      m_swap_chain;
    ComPtr<ID3D12DescriptorHeap> m_rtv_heap;

    ComPtr<ID3D12Resource>  m_back_buffers[k_frame_count];
    uint32_t                m_rtv_descriptor_size = 0;

    HdrMode  m_hdr_mode = HdrMode::SDR;
    DXGI_FORMAT m_back_buffer_format = DXGI_FORMAT_R8G8B8A8_UNORM;

    uint32_t m_width   = 0;
    uint32_t m_height  = 0;

    bool m_initialised = false;
};

} // namespace mars
