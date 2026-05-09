// =============================================================================
// device_context.h
// MARS 3D Engine — D3D12 Device Context
//
// Owns the D3D12 device, adapter, and the three command queues
// (Direct, Compute, Copy).  Also manages the bindless descriptor heap
// and provides fence-based CPU/GPU synchronisation helpers.
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../d3d12_agility.h"

#include <wrl/client.h>
#include <cstdint>
#include <string>

namespace mars
{

using Microsoft::WRL::ComPtr;

// Number of frames kept in flight simultaneously (back-buffer count).
static constexpr uint32_t k_frame_count = 2;

// Bindless heap capacity: 1 000 000 descriptors (CBV/SRV/UAV).
// D3D12 enforces a hard limit of 1 000 000 for shader-visible CBV/SRV/UAV heaps.
static constexpr uint32_t k_bindless_heap_size = 1'000'000;

// =============================================================================
// FenceSync — lightweight GPU fence + event handle
// =============================================================================
struct FenceSync
{
    ComPtr<ID3D12Fence> fence;
    HANDLE              event  = nullptr;
    uint64_t            value  = 0;

    bool init(ID3D12Device* device, const wchar_t* debug_name);
    void destroy();

    // Signal the fence from the CPU side and wait until the GPU passes it.
    void signal_and_wait(ID3D12CommandQueue* queue);

    // Insert a GPU-side signal and return the value to wait on later.
    uint64_t signal(ID3D12CommandQueue* queue);

    // Block the calling thread until the GPU reaches `wait_value`.
    void cpu_wait(uint64_t wait_value) const;
};

// =============================================================================
// DeviceContext
// =============================================================================
class MARS_ENGINE_API DeviceContext
{
public:
    DeviceContext()  = default;
    ~DeviceContext() { shutdown(); }

    // Non-copyable, non-movable (owns raw OS handles).
    DeviceContext(const DeviceContext&)            = delete;
    DeviceContext& operator=(const DeviceContext&) = delete;

    // Initialise device, queues, and bindless heap.
    // Throws std::runtime_error on failure.
    void init();
    void shutdown();

    // ---- Accessors ----------------------------------------------------------
    ID3D12Device5*       device()         const { return m_device.Get(); }
    IDXGIFactory6*       dxgi_factory()   const { return m_dxgi_factory.Get(); }
    IDXGIAdapter1*       adapter()        const { return m_adapter.Get(); }

    ID3D12CommandQueue*  direct_queue()   const { return m_direct_queue.Get(); }
    ID3D12CommandQueue*  compute_queue()  const { return m_compute_queue.Get(); }
    ID3D12CommandQueue*  copy_queue()     const { return m_copy_queue.Get(); }

    // Bindless CBV/SRV/UAV heap (shader-visible, GPU-side).
    ID3D12DescriptorHeap* bindless_heap() const { return m_bindless_heap.Get(); }
    uint32_t              bindless_descriptor_size() const { return m_bindless_descriptor_size; }

    // Allocate the next free slot in the bindless heap.
    // Returns the slot index (used to build GPU descriptor handles).
    uint32_t allocate_bindless_slot();

    // Flush all three queues and wait for the GPU to be idle.
    void flush_gpu();

private:
    void create_dxgi_factory();
    void select_adapter();
    void create_device();
    void check_dxr_support();
    void create_command_queues();
    void create_bindless_heap();

    ComPtr<IDXGIFactory6>       m_dxgi_factory;
    ComPtr<IDXGIAdapter1>       m_adapter;
    ComPtr<ID3D12Device5>       m_device;

    ComPtr<ID3D12CommandQueue>  m_direct_queue;
    ComPtr<ID3D12CommandQueue>  m_compute_queue;
    ComPtr<ID3D12CommandQueue>  m_copy_queue;

    FenceSync  m_direct_fence;
    FenceSync  m_compute_fence;
    FenceSync  m_copy_fence;

    ComPtr<ID3D12DescriptorHeap> m_bindless_heap;
    uint32_t                     m_bindless_descriptor_size = 0;
    uint32_t                     m_bindless_next_slot       = 0;

    bool m_initialised = false;
};

} // namespace mars
