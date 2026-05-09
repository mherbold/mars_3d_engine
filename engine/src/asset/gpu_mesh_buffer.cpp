// =============================================================================
// gpu_mesh_buffer.cpp
// MARS 3D Engine — GPU vertex / index buffer upload implementation
// =============================================================================

#include "mars_engine/asset/gpu_mesh_buffer.h"
#include "mars_engine/renderer/device_context.h"

#pragma warning(push, 0)
#include <D3D12MemAlloc.h>
#pragma warning(pop)

#include <stdexcept>
#include <format>
#include <cstring>

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

// Upload `byte_size` bytes from `data` into a DEFAULT-heap resource via a
// temporary UPLOAD-heap resource.  Blocks until the copy queue finishes.
static void upload_buffer(
    DeviceContext&         ctx,
    D3D12MA::Allocator*    allocator,
    ID3D12Resource*        dest,
    const void*            data,
    uint64_t               byte_size)
{
    // ---- Create a temporary upload buffer ----
    D3D12MA::ALLOCATION_DESC upload_desc{};
    upload_desc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC buf_desc = CD3DX12_RESOURCE_DESC::Buffer(byte_size);

    D3D12MA::Allocation* upload_alloc = nullptr;
    ID3D12Resource*      upload_buf   = nullptr;

    throw_if_failed(
        allocator->CreateResource(
            &upload_desc,
            &buf_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            &upload_alloc,
            IID_PPV_ARGS(&upload_buf)),
        "D3D12MA: CreateResource (upload buffer) failed");

    // Map and copy CPU data into upload buffer.
    void* mapped = nullptr;
    throw_if_failed(upload_buf->Map(0, nullptr, &mapped), "Upload buffer Map failed");
    std::memcpy(mapped, data, static_cast<size_t>(byte_size));
    upload_buf->Unmap(0, nullptr);

    // ---- Record and submit a copy command ----
    ComPtr<ID3D12CommandAllocator> cmd_alloc;
    throw_if_failed(
        ctx.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&cmd_alloc)),
        "CreateCommandAllocator (copy) failed");

    ComPtr<ID3D12GraphicsCommandList> cmd_list;
    throw_if_failed(
        ctx.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
            cmd_alloc.Get(), nullptr, IID_PPV_ARGS(&cmd_list)),
        "CreateCommandList (copy) failed");

    cmd_list->CopyBufferRegion(dest, 0, upload_buf, 0, byte_size);
    throw_if_failed(cmd_list->Close(), "Copy command list Close failed");

    ID3D12CommandList* lists[] = {cmd_list.Get()};
    ctx.copy_queue()->ExecuteCommandLists(1, lists);

    // Block until the copy is complete using a temporary fence.
    ComPtr<ID3D12Fence> fence;
    HANDLE              evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    throw_if_failed(
        ctx.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
        "CreateFence (upload) failed");
    throw_if_failed(ctx.copy_queue()->Signal(fence.Get(), 1), "Signal (upload) failed");
    if (fence->GetCompletedValue() < 1)
    {
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObjectEx(evt, INFINITE, FALSE);
    }
    CloseHandle(evt);

    // Release temporary resources.
    upload_buf->Release();
    upload_alloc->Release();
}

// ---------------------------------------------------------------------------
// GpuMeshBuffer::upload
// ---------------------------------------------------------------------------
void GpuMeshBuffer::upload(DeviceContext& ctx, D3D12MA::Allocator* allocator, const MeshData& mesh)
{
    m_vertex_count = static_cast<uint32_t>(mesh.vertices.size());
    m_index_count  = static_cast<uint32_t>(mesh.indices.size());
    m_bounds       = mesh.bounds;

    const uint64_t vb_size = static_cast<uint64_t>(m_vertex_count) * sizeof(Vertex);
    const uint64_t ib_size = static_cast<uint64_t>(m_index_count)  * sizeof(uint32_t);

    // ---- Create DEFAULT-heap vertex buffer ----
    {
        D3D12MA::ALLOCATION_DESC alloc_desc{};
        alloc_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC buf_desc = CD3DX12_RESOURCE_DESC::Buffer(
            vb_size, D3D12_RESOURCE_FLAG_NONE);

        throw_if_failed(
            allocator->CreateResource(
                &alloc_desc,
                &buf_desc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                &m_vertex_alloc,
                IID_PPV_ARGS(&m_vertex_buffer)),
            "D3D12MA: CreateResource (vertex buffer) failed");

        m_vertex_buffer->SetName(L"GpuMeshBuffer::VertexBuffer");
    }

    // ---- Create DEFAULT-heap index buffer ----
    {
        D3D12MA::ALLOCATION_DESC alloc_desc{};
        alloc_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC buf_desc = CD3DX12_RESOURCE_DESC::Buffer(
            ib_size, D3D12_RESOURCE_FLAG_NONE);

        throw_if_failed(
            allocator->CreateResource(
                &alloc_desc,
                &buf_desc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                &m_index_alloc,
                IID_PPV_ARGS(&m_index_buffer)),
            "D3D12MA: CreateResource (index buffer) failed");

        m_index_buffer->SetName(L"GpuMeshBuffer::IndexBuffer");
    }

    // ---- Upload data via copy queue ----
    upload_buffer(ctx, allocator, m_vertex_buffer, mesh.vertices.data(), vb_size);
    upload_buffer(ctx, allocator, m_index_buffer,  mesh.indices.data(),  ib_size);

    // ---- Build views ----
    m_vbv.BufferLocation = m_vertex_buffer->GetGPUVirtualAddress();
    m_vbv.SizeInBytes    = static_cast<UINT>(vb_size);
    m_vbv.StrideInBytes  = sizeof(Vertex);

    m_ibv.BufferLocation = m_index_buffer->GetGPUVirtualAddress();
    m_ibv.SizeInBytes    = static_cast<UINT>(ib_size);
    m_ibv.Format         = DXGI_FORMAT_R32_UINT;

    // ---- Register vertex buffer SRV in the bindless heap ----
    m_vertex_srv_slot = ctx.allocate_bindless_slot();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format                     = DXGI_FORMAT_UNKNOWN;
    srv_desc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srv_desc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Buffer.FirstElement        = 0;
    srv_desc.Buffer.NumElements         = m_vertex_count;
    srv_desc.Buffer.StructureByteStride = sizeof(Vertex);
    srv_desc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle =
        ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
    cpu_handle.ptr += static_cast<SIZE_T>(m_vertex_srv_slot) * ctx.bindless_descriptor_size();

    ctx.device()->CreateShaderResourceView(m_vertex_buffer, &srv_desc, cpu_handle);
}

// ---------------------------------------------------------------------------
// GpuMeshBuffer::destroy
// ---------------------------------------------------------------------------
void GpuMeshBuffer::destroy()
{
    if (m_vertex_buffer) { m_vertex_buffer->Release(); m_vertex_buffer = nullptr; }
    if (m_index_buffer)  { m_index_buffer->Release();  m_index_buffer  = nullptr; }
    if (m_vertex_alloc)  { m_vertex_alloc->Release();  m_vertex_alloc  = nullptr; }
    if (m_index_alloc)   { m_index_alloc->Release();   m_index_alloc   = nullptr; }
    m_vbv = {};
    m_ibv = {};
    m_vertex_count    = 0;
    m_index_count     = 0;
    m_vertex_srv_slot = UINT32_MAX;
}

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------
GpuMeshBuffer::GpuMeshBuffer(GpuMeshBuffer&& o) noexcept
    : m_vertex_alloc(o.m_vertex_alloc)
    , m_index_alloc(o.m_index_alloc)
    , m_vertex_buffer(o.m_vertex_buffer)
    , m_index_buffer(o.m_index_buffer)
    , m_vbv(o.m_vbv)
    , m_ibv(o.m_ibv)
    , m_vertex_count(o.m_vertex_count)
    , m_index_count(o.m_index_count)
    , m_vertex_srv_slot(o.m_vertex_srv_slot)
    , m_bounds(o.m_bounds)
{
    o.m_vertex_alloc  = nullptr;
    o.m_index_alloc   = nullptr;
    o.m_vertex_buffer = nullptr;
    o.m_index_buffer  = nullptr;
    o.m_vertex_count  = 0;
    o.m_index_count   = 0;
    o.m_vertex_srv_slot = UINT32_MAX;
}

GpuMeshBuffer& GpuMeshBuffer::operator=(GpuMeshBuffer&& o) noexcept
{
    if (this != &o)
    {
        destroy();
        new (this) GpuMeshBuffer(std::move(o));
    }
    return *this;
}

} // namespace mars
