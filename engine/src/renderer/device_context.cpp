// =============================================================================
// device_context.cpp
// MARS 3D Engine — D3D12 Device Context implementation
// =============================================================================

#include "mars_engine/renderer/device_context.h"

#include <stdexcept>
#include <string>
#include <format>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

namespace mars
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void throw_if_failed(HRESULT hr, const char* msg)
{
    if (FAILED(hr))
        throw std::runtime_error(std::format("{} (HRESULT 0x{:08X})", msg, static_cast<unsigned>(hr)));
}

// ---------------------------------------------------------------------------
// FenceSync
// ---------------------------------------------------------------------------
bool FenceSync::init(ID3D12Device* device, const wchar_t* debug_name)
{
    value = 0;
    HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) return false;
    fence->SetName(debug_name);
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return event != nullptr;
}

void FenceSync::destroy()
{
    if (event) { CloseHandle(event); event = nullptr; }
    fence.Reset();
    value = 0;
}

uint64_t FenceSync::signal(ID3D12CommandQueue* queue)
{
    ++value;
    queue->Signal(fence.Get(), value);
    return value;
}

void FenceSync::cpu_wait(uint64_t wait_value) const
{
    if (fence->GetCompletedValue() < wait_value)
    {
        fence->SetEventOnCompletion(wait_value, event);
        WaitForSingleObjectEx(event, INFINITE, FALSE);
    }
}

void FenceSync::signal_and_wait(ID3D12CommandQueue* queue)
{
    cpu_wait(signal(queue));
}

// ---------------------------------------------------------------------------
// DeviceContext — private helpers
// ---------------------------------------------------------------------------
void DeviceContext::create_dxgi_factory()
{
    UINT flags = 0;
#if defined(_DEBUG)
    // Enable D3D12 debug layer before creating the device.
    {
        ComPtr<ID3D12Debug3> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            debug->SetEnableGPUBasedValidation(TRUE);
        }
        flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    throw_if_failed(
        CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_dxgi_factory)),
        "CreateDXGIFactory2 failed");
}

void DeviceContext::select_adapter()
{
    // Prefer a hardware adapter with the most video memory.
    ComPtr<IDXGIAdapter1> candidate;
    SIZE_T best_vram = 0;

    for (UINT i = 0;
         m_dxgi_factory->EnumAdapterByGpuPreference(
             i,
             DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&candidate)) != DXGI_ERROR_NOT_FOUND;
         ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);

        // Skip Microsoft Basic Render Driver (software adapter).
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        // Verify the adapter can create a D3D12 device (no actual device yet).
        if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_1, __uuidof(ID3D12Device), nullptr)))
            continue;

        if (desc.DedicatedVideoMemory > best_vram)
        {
            best_vram = desc.DedicatedVideoMemory;
            m_adapter = candidate;
        }
    }

    if (!m_adapter)
        throw std::runtime_error("No D3D12-capable hardware adapter found.");
}

void DeviceContext::create_device()
{
    throw_if_failed(
        D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&m_device)),
        "D3D12CreateDevice failed (feature level 12_1)");

    m_device->SetName(L"MARS::D3D12Device");

#if defined(_DEBUG)
    ComPtr<ID3D12InfoQueue1> info_queue;
    if (SUCCEEDED(m_device.As(&info_queue)))
    {
        info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR,      TRUE);
        info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING,    FALSE);

        // Streamline (sl.dlss_d) internally manages the mvec resource state and
        // intentionally issues barriers that the D3D12 debug layer reports as
        // RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH (#527). This is a known false
        // positive from Streamline's internal resource management — suppress the
        // debug break for this specific message ID to allow DLSS-RR to run.
        D3D12_MESSAGE_ID suppressed_ids[] = {
            D3D12_MESSAGE_ID_RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH,
        };
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumIDs  = static_cast<UINT>(std::size(suppressed_ids));
        filter.DenyList.pIDList = suppressed_ids;
        info_queue->AddStorageFilterEntries(&filter);
    }
#endif
}

void DeviceContext::check_dxr_support()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
    HRESULT hr = m_device->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5));

    if (FAILED(hr) || opts5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
        throw std::runtime_error(
            "GPU does not support DXR Tier 1.1, which is required by MARS.");
}

void DeviceContext::create_command_queues()
{
    auto make_queue = [&](D3D12_COMMAND_LIST_TYPE type,
                          const wchar_t* name,
                          ComPtr<ID3D12CommandQueue>& out_queue,
                          FenceSync& out_fence)
    {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type     = type;
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        desc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        desc.NodeMask = 0;

        throw_if_failed(
            m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&out_queue)),
            "CreateCommandQueue failed");
        out_queue->SetName(name);

        if (!out_fence.init(m_device.Get(), name))
            throw std::runtime_error("FenceSync::init failed");
    };

    make_queue(D3D12_COMMAND_LIST_TYPE_DIRECT,  L"MARS::DirectQueue",  m_direct_queue,  m_direct_fence);
    make_queue(D3D12_COMMAND_LIST_TYPE_COMPUTE, L"MARS::ComputeQueue", m_compute_queue, m_compute_fence);
    make_queue(D3D12_COMMAND_LIST_TYPE_COPY,    L"MARS::CopyQueue",    m_copy_queue,    m_copy_fence);
}

void DeviceContext::create_bindless_heap()
{
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = k_bindless_heap_size;
    desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    desc.NodeMask       = 0;

    throw_if_failed(
        m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_bindless_heap)),
        "CreateDescriptorHeap (bindless) failed");

    m_bindless_heap->SetName(L"MARS::BindlessHeap");
    m_bindless_descriptor_size =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_bindless_next_slot = 0;
}

// ---------------------------------------------------------------------------
// DeviceContext — public API
// ---------------------------------------------------------------------------
void DeviceContext::init()
{
    if (m_initialised) return;

    create_dxgi_factory();
    select_adapter();
    create_device();
    check_dxr_support();
    create_command_queues();
    create_bindless_heap();

    m_initialised = true;
}

void DeviceContext::shutdown()
{
    if (!m_initialised) return;
    flush_gpu();

    m_bindless_heap.Reset();

    m_direct_fence.destroy();
    m_compute_fence.destroy();
    m_copy_fence.destroy();

    m_direct_queue.Reset();
    m_compute_queue.Reset();
    m_copy_queue.Reset();

    m_device.Reset();
    m_adapter.Reset();
    m_dxgi_factory.Reset();

    m_initialised = false;
}

uint32_t DeviceContext::allocate_bindless_slot()
{
    uint32_t slot = m_bindless_next_slot++;
    if (slot >= k_bindless_heap_size)
        throw std::runtime_error("Bindless heap exhausted.");
    return slot;
}

void DeviceContext::flush_gpu()
{
    m_direct_fence.signal_and_wait(m_direct_queue.Get());
    m_compute_fence.signal_and_wait(m_compute_queue.Get());
    m_copy_fence.signal_and_wait(m_copy_queue.Get());
}

} // namespace mars
