// =============================================================================
// renderer.cpp
// MARS 3D Engine — Renderer implementation (M2: multi-monitor clear + present)
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
// Public init overloads
// ---------------------------------------------------------------------------

// Single-monitor convenience (M1 API preserved).
void Renderer::init(HWND hwnd, uint32_t width, uint32_t height)
{
    DisplayConfig cfg;
    cfg.width  = width;
    cfg.height = height;
    init_internal({ cfg }, { hwnd });
}

// Multi-monitor init: load display.json, create one output per entry.
void Renderer::init(const std::string& display_json_path, const std::vector<HWND>& hwnds)
{
    std::vector<DisplayConfig> configs;
    m_display_manager.load_config(display_json_path, configs);

    // If json was empty/missing, default to one output per provided HWND.
    if (configs.empty())
    {
        for (uint32_t i = 0; i < static_cast<uint32_t>(hwnds.size()); ++i)
        {
            DisplayConfig cfg;
            cfg.monitor_index = i;
            configs.push_back(cfg);
        }
    }

    // Ensure we have enough HWNDs: duplicate the last one if needed.
    std::vector<HWND> padded_hwnds = hwnds;
    while (padded_hwnds.size() < configs.size())
        padded_hwnds.push_back(padded_hwnds.back());

    init_internal(configs, padded_hwnds);
}

// ---------------------------------------------------------------------------
// init_internal (private) — common path for both overloads
// ---------------------------------------------------------------------------
void Renderer::init_internal(const std::vector<DisplayConfig>& configs,
                              const std::vector<HWND>&          hwnds)
{
    if (m_initialised) return;

    m_device_ctx.init();
    m_display_manager.init(m_device_ctx, configs, hwnds);
    create_frame_resources();

    m_initialised = true;
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------
void Renderer::shutdown()
{
    if (!m_initialised) return;

    m_device_ctx.flush_gpu();
    for (uint32_t i = 0; i < k_frame_count; ++i)
        wait_for_frame(i);

    release_frame_resources();
    m_display_manager.shutdown();
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

    throw_if_failed(
        device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_cmd_allocators[0].Get(),
            nullptr,
            IID_PPV_ARGS(&m_cmd_list)),
        "CreateCommandList failed");
    m_cmd_list->SetName(L"MARS::MainCmdList");
    m_cmd_list->Close();

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
// on_resize overloads
// ---------------------------------------------------------------------------
void Renderer::on_resize(uint32_t output_index, uint32_t width, uint32_t height)
{
    m_device_ctx.flush_gpu();
    m_display_manager.resize(output_index, width, height);
}

void Renderer::on_resize(uint32_t width, uint32_t height)
{
    on_resize(0, width, height);
}

// ---------------------------------------------------------------------------
// render_frame  (M2: clear all outputs to distinct colors, then present each)
//
// Each DisplayOutput is independent — we record a separate command list
// submission for each monitor so they can have different back-buffer indices
// in flight simultaneously.  (For M2 all outputs share the same k_frame_count
// fencing; a future milestone can give each output its own fence ring.)
// ---------------------------------------------------------------------------
void Renderer::render_frame()
{
    // All outputs share the same command allocator ring for now.
    // The back-buffer index for fencing comes from the primary output (index 0).
    uint32_t back_index = (m_display_manager.output_count() > 0)
        ? m_display_manager.output(0).current_back_buffer_index()
        : 0;

    wait_for_frame(back_index);

    throw_if_failed(m_cmd_allocators[back_index]->Reset(), "CommandAllocator::Reset failed");
    throw_if_failed(m_cmd_list->Reset(m_cmd_allocators[back_index].Get(), nullptr),
                    "CommandList::Reset failed");

    // Distinct clear colors per output role so multi-monitor is visually obvious.
    static constexpr FLOAT k_clear_colors[][4] = {
        { 0.01f, 0.05f, 0.15f, 1.0f },  // Center  — deep navy
        { 0.12f, 0.04f, 0.04f, 1.0f },  // Left    — deep red
        { 0.04f, 0.12f, 0.04f, 1.0f },  // Right   — deep green
        { 0.10f, 0.08f, 0.02f, 1.0f },  // Overhead— deep amber
        { 0.05f, 0.05f, 0.05f, 1.0f },  // Custom / extra
    };

    for (uint32_t oi = 0; oi < m_display_manager.output_count(); ++oi)
    {
        DisplayOutput& out = m_display_manager.output(oi);

        out.transition(m_cmd_list.Get(),
                       D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);

        const FLOAT* color = k_clear_colors[
            oi < std::size(k_clear_colors) ? oi : (std::size(k_clear_colors) - 1)];
        auto rtv = out.current_rtv();
        m_cmd_list->ClearRenderTargetView(rtv, color, 0, nullptr);

        out.transition(m_cmd_list.Get(),
                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PRESENT);
    }

    throw_if_failed(m_cmd_list->Close(), "CommandList::Close failed");

    ID3D12CommandList* lists[] = { m_cmd_list.Get() };
    m_device_ctx.direct_queue()->ExecuteCommandLists(1, lists);

    // Present all outputs.
    for (uint32_t oi = 0; oi < m_display_manager.output_count(); ++oi)
        m_display_manager.output(oi).present(true);

    ++m_frame_fence_next;
    m_device_ctx.direct_queue()->Signal(m_frame_fence.Get(), m_frame_fence_next);
    m_frame_fence_values[back_index] = m_frame_fence_next;
}

} // namespace mars
