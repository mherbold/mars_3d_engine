// =============================================================================
// display_output.cpp
// MARS 3D Engine — Display Output implementation
// =============================================================================

#include "mars_engine/renderer/display_output.h"

#include <stdexcept>
#include <format>

namespace mars
{

static void throw_if_failed(HRESULT hr, const char* msg)
{
    if (FAILED(hr))
        throw std::runtime_error(std::format("{} (HRESULT 0x{:08X})", msg, static_cast<unsigned>(hr)));
}

// ---------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------
void DisplayOutput::init(DeviceContext& device_ctx, HWND hwnd, uint32_t width, uint32_t height)
{
    if (m_initialised) return;

    m_device_ctx = &device_ctx;
    m_width      = width;
    m_height     = height;

    create_swap_chain(hwnd);
    detect_hdr_support();
    create_rtvs();

    m_initialised = true;
}

void DisplayOutput::shutdown()
{
    if (!m_initialised) return;
    release_rtvs();
    m_swap_chain.Reset();
    m_rtv_heap.Reset();
    m_device_ctx   = nullptr;
    m_initialised  = false;
}

// ---------------------------------------------------------------------------
// create_swap_chain
// ---------------------------------------------------------------------------
void DisplayOutput::create_swap_chain(HWND hwnd)
{
    // Default to SDR format; may be upgraded after HDR detection.
    m_back_buffer_format = DXGI_FORMAT_R8G8B8A8_UNORM;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width       = m_width;
    desc.Height      = m_height;
    desc.Format      = m_back_buffer_format;
    desc.Stereo      = FALSE;
    desc.SampleDesc  = { 1, 0 };
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = k_frame_count;
    desc.Scaling     = DXGI_SCALING_STRETCH;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags       = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    ComPtr<IDXGISwapChain1> swap_chain1;
    throw_if_failed(
        m_device_ctx->dxgi_factory()->CreateSwapChainForHwnd(
            m_device_ctx->direct_queue(),
            hwnd,
            &desc,
            nullptr,    // no full-screen desc
            nullptr,    // no output restriction
            &swap_chain1),
        "CreateSwapChainForHwnd failed");

    // Disable the automatic Alt+Enter full-screen shortcut (we manage it).
    m_device_ctx->dxgi_factory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    throw_if_failed(
        swap_chain1.As(&m_swap_chain),
        "QueryInterface IDXGISwapChain4 failed");
}

// ---------------------------------------------------------------------------
// detect_hdr_support
// Checks the output the swap chain is on and selects the best color space.
// ---------------------------------------------------------------------------
void DisplayOutput::detect_hdr_support()
{
    // Get the output (monitor) the swap chain is currently on.
    ComPtr<IDXGIOutput> output;
    if (FAILED(m_swap_chain->GetContainingOutput(&output)))
        return;  // Can't determine — stay SDR.

    ComPtr<IDXGIOutput6> output6;
    if (FAILED(output.As(&output6)))
        return;

    DXGI_OUTPUT_DESC1 out_desc{};
    if (FAILED(output6->GetDesc1(&out_desc)))
        return;

    // HDR10: the monitor reports a wide colour gamut and high max luminance.
    if (out_desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
    {
        // Prefer scRGB (16-bit float) on Windows HDR for full flexibility.
        UINT color_space_support = 0;
        if (SUCCEEDED(m_swap_chain->CheckColorSpaceSupport(
                DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &color_space_support)) &&
            (color_space_support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        {
            m_swap_chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
            m_back_buffer_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            m_hdr_mode = HdrMode::scRGB;
            return;
        }

        // Fall back to HDR10 / PQ if scRGB is not supported.
        if (SUCCEEDED(m_swap_chain->CheckColorSpaceSupport(
                DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &color_space_support)) &&
            (color_space_support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        {
            m_swap_chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            m_back_buffer_format = DXGI_FORMAT_R10G10B10A2_UNORM;
            m_hdr_mode = HdrMode::HDR10;
            return;
        }
    }

    // SDR fallback — leave format and color space at defaults.
    m_hdr_mode = HdrMode::SDR;
}

// ---------------------------------------------------------------------------
// create_rtvs / release_rtvs
// ---------------------------------------------------------------------------
void DisplayOutput::create_rtvs()
{
    auto* device = m_device_ctx->device();

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = k_frame_count;
    heap_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    throw_if_failed(
        device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_rtv_heap)),
        "CreateDescriptorHeap (RTV) failed");

    m_rtv_heap->SetName(L"MARS::DisplayOutput::RTVHeap");
    m_rtv_descriptor_size =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    auto rtv_handle = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t i = 0; i < k_frame_count; ++i)
    {
        throw_if_failed(
            m_swap_chain->GetBuffer(i, IID_PPV_ARGS(&m_back_buffers[i])),
            "GetBuffer failed");

        device->CreateRenderTargetView(m_back_buffers[i].Get(), nullptr, rtv_handle);
        rtv_handle.ptr += m_rtv_descriptor_size;
    }
}

void DisplayOutput::release_rtvs()
{
    for (auto& buf : m_back_buffers)
        buf.Reset();
    m_rtv_heap.Reset();
}

// ---------------------------------------------------------------------------
// Per-frame helpers
// ---------------------------------------------------------------------------
uint32_t DisplayOutput::current_back_buffer_index() const
{
    return m_swap_chain->GetCurrentBackBufferIndex();
}

ID3D12Resource* DisplayOutput::current_back_buffer() const
{
    return m_back_buffers[current_back_buffer_index()].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE DisplayOutput::current_rtv() const
{
    auto handle = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(current_back_buffer_index()) * m_rtv_descriptor_size;
    return handle;
}

void DisplayOutput::transition(ID3D12GraphicsCommandList* cmd_list,
                               D3D12_RESOURCE_STATES from,
                               D3D12_RESOURCE_STATES to) const
{
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        current_back_buffer(), from, to);
    cmd_list->ResourceBarrier(1, &barrier);
}

void DisplayOutput::present(bool vsync)
{
    UINT sync_interval = vsync ? 1 : 0;
    UINT flags         = vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING;
    m_swap_chain->Present(sync_interval, flags);
}

// ---------------------------------------------------------------------------
// resize
// ---------------------------------------------------------------------------
void DisplayOutput::resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;

    m_device_ctx->flush_gpu();
    release_rtvs();

    m_width  = width;
    m_height = height;

    throw_if_failed(
        m_swap_chain->ResizeBuffers(
            k_frame_count, width, height,
            m_back_buffer_format,
            DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING),
        "ResizeBuffers failed");

    create_rtvs();
}

} // namespace mars
