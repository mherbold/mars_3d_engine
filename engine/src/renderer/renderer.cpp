// =============================================================================
// renderer.cpp
// MARS 3D Engine — Renderer implementation (M1: clear + present)
// =============================================================================

#include "mars_engine/renderer/renderer.h"

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
void Renderer::init(HWND hwnd, uint32_t width, uint32_t height)
{
    if (m_initialised) return;

    m_device_ctx.init();
    m_display_output.init(m_device_ctx, hwnd, width, height);
    create_frame_resources();

    m_initialised = true;
}

void Renderer::shutdown()
{
    if (!m_initialised) return;

    // Wait for all in-flight frames before releasing resources.
    m_device_ctx.flush_gpu();
    for (uint32_t i = 0; i < k_frame_count; ++i)
        wait_for_frame(i);

    release_frame_resources();
    m_display_output.shutdown();
    m_device_ctx.shutdown();

    m_initialised = false;
}

// ---------------------------------------------------------------------------
// Frame resources
// ---------------------------------------------------------------------------
void Renderer::create_frame_resources()
{
    auto* device = m_device_ctx.device();

    for (uint32_t i = 0; i < k_frame_count; ++i)
    {
        throw_if_failed(
            device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&m_cmd_allocators[i])),
            "CreateCommandAllocator failed");

        m_cmd_allocators[i]->SetName(
            std::wstring(L"MARS::CmdAlloc[" + std::to_wstring(i) + L"]").c_str());

        m_frame_fence_values[i] = 0;
    }

    // Single re-usable command list (reset each frame with the matching allocator).
    throw_if_failed(
        device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_cmd_allocators[0].Get(),
            nullptr,
            IID_PPV_ARGS(&m_cmd_list)),
        "CreateCommandList failed");
    m_cmd_list->SetName(L"MARS::MainCmdList");
    m_cmd_list->Close();  // Start in closed state; we reset before first use.

    // Frame fence (signals after each Present).
    throw_if_failed(
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_frame_fence)),
        "CreateFence (frame) failed");
    m_frame_fence->SetName(L"MARS::FrameFence");
    m_frame_fence_next  = 0;
    m_frame_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_frame_fence_event)
        throw std::runtime_error("CreateEvent (frame fence) failed");
}

void Renderer::release_frame_resources()
{
    m_cmd_list.Reset();
    for (auto& alloc : m_cmd_allocators)
        alloc.Reset();
    m_frame_fence.Reset();
    if (m_frame_fence_event) { CloseHandle(m_frame_fence_event); m_frame_fence_event = nullptr; }
}

void Renderer::wait_for_frame(uint32_t frame_index)
{
    uint64_t target = m_frame_fence_values[frame_index];
    if (target == 0) return;
    if (m_frame_fence->GetCompletedValue() < target)
    {
        m_frame_fence->SetEventOnCompletion(target, m_frame_fence_event);
        WaitForSingleObjectEx(m_frame_fence_event, INFINITE, FALSE);
    }
}

// ---------------------------------------------------------------------------
// on_resize
// ---------------------------------------------------------------------------
void Renderer::on_resize(uint32_t width, uint32_t height)
{
    m_device_ctx.flush_gpu();
    m_display_output.resize(width, height);
}

// ---------------------------------------------------------------------------
// render_frame  (M1: clear back buffer to a color, present)
// ---------------------------------------------------------------------------
void Renderer::render_frame()
{
    uint32_t back_index = m_display_output.current_back_buffer_index();

    // Wait for the GPU to finish using this back buffer's allocator.
    wait_for_frame(back_index);

    // Reset command allocator + list for this frame.
    throw_if_failed(m_cmd_allocators[back_index]->Reset(), "CommandAllocator::Reset failed");
    throw_if_failed(m_cmd_list->Reset(m_cmd_allocators[back_index].Get(), nullptr),
                    "CommandList::Reset failed");

    // Transition back buffer: Present → Render Target.
    m_display_output.transition(m_cmd_list.Get(),
                                D3D12_RESOURCE_STATE_PRESENT,
                                D3D12_RESOURCE_STATE_RENDER_TARGET);

    // Clear to a deep navy color (visible, non-black, easy to confirm).
    const FLOAT clear_color[4] = { 0.01f, 0.05f, 0.15f, 1.0f };
    auto rtv = m_display_output.current_rtv();
    m_cmd_list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);

    // Transition back buffer: Render Target → Present.
    m_display_output.transition(m_cmd_list.Get(),
                                D3D12_RESOURCE_STATE_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_PRESENT);

    throw_if_failed(m_cmd_list->Close(), "CommandList::Close failed");

    // Execute.
    ID3D12CommandList* lists[] = { m_cmd_list.Get() };
    m_device_ctx.direct_queue()->ExecuteCommandLists(1, lists);

    // Present.
    m_display_output.present(true);

    // Signal frame fence so we know when the GPU is done with this frame.
    ++m_frame_fence_next;
    m_device_ctx.direct_queue()->Signal(m_frame_fence.Get(), m_frame_fence_next);
    m_frame_fence_values[back_index] = m_frame_fence_next;
}

} // namespace mars
