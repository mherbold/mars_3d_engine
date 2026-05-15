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
#include <cassert>
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

// GPU-to-GPU copy of `byte_size` bytes from `src` into `dest` via the copy
// queue.  Both resources must be on the DEFAULT heap.  Blocks until done.
static void copy_buffer(
    DeviceContext&  ctx,
    ID3D12Resource* dest,
    ID3D12Resource* src,
    uint64_t        byte_size)
{
    ComPtr<ID3D12CommandAllocator> cmd_alloc;
    throw_if_failed(
        ctx.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&cmd_alloc)),
        "CreateCommandAllocator (gpu copy) failed");

    ComPtr<ID3D12GraphicsCommandList> cmd_list;
    throw_if_failed(
        ctx.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
            cmd_alloc.Get(), nullptr, IID_PPV_ARGS(&cmd_list)),
        "CreateCommandList (gpu copy) failed");

    cmd_list->CopyBufferRegion(dest, 0, src, 0, byte_size);
    throw_if_failed(cmd_list->Close(), "Copy command list Close failed");

    ID3D12CommandList* lists[] = {cmd_list.Get()};
    ctx.copy_queue()->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    HANDLE              evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    throw_if_failed(
        ctx.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
        "CreateFence (gpu copy) failed");
    throw_if_failed(ctx.copy_queue()->Signal(fence.Get(), 1), "Signal (gpu copy) failed");
    if (fence->GetCompletedValue() < 1)
    {
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObjectEx(evt, INFINITE, FALSE);
    }
    CloseHandle(evt);
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

    // ---- Register vertex buffer SRV in the bindless heap (ByteAddressBuffer) ----
    // The path-trace shader reads vertices via g_Buffers[vbSrv].Load*() (raw byte access).
    m_vertex_srv_slot = ctx.allocate_bindless_slot();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format                     = DXGI_FORMAT_R32_TYPELESS;
    srv_desc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srv_desc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Buffer.FirstElement        = 0;
    srv_desc.Buffer.NumElements         = static_cast<UINT>(vb_size / 4); // count in DWORDs
    srv_desc.Buffer.StructureByteStride = 0;
    srv_desc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_RAW;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle =
        ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
    cpu_handle.ptr += static_cast<SIZE_T>(m_vertex_srv_slot) * ctx.bindless_descriptor_size();

    ctx.device()->CreateShaderResourceView(m_vertex_buffer, &srv_desc, cpu_handle);

    // ---- Register index buffer SRV in the bindless heap (ByteAddressBuffer) ----
    m_index_srv_slot = ctx.allocate_bindless_slot();

    D3D12_SHADER_RESOURCE_VIEW_DESC ib_srv_desc{};
    ib_srv_desc.Format                    = DXGI_FORMAT_R32_TYPELESS;
    ib_srv_desc.ViewDimension             = D3D12_SRV_DIMENSION_BUFFER;
    ib_srv_desc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ib_srv_desc.Buffer.FirstElement       = 0;
    ib_srv_desc.Buffer.NumElements        = m_index_count;
    ib_srv_desc.Buffer.StructureByteStride= 0;
    ib_srv_desc.Buffer.Flags              = D3D12_BUFFER_SRV_FLAG_RAW;

    D3D12_CPU_DESCRIPTOR_HANDLE ib_cpu_handle =
        ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
    ib_cpu_handle.ptr += static_cast<SIZE_T>(m_index_srv_slot) * ctx.bindless_descriptor_size();

    ctx.device()->CreateShaderResourceView(m_index_buffer, &ib_srv_desc, ib_cpu_handle);
}

// ---------------------------------------------------------------------------
// GpuMeshBuffer::enable_skinning
// ---------------------------------------------------------------------------
void GpuMeshBuffer::enable_skinning(DeviceContext& ctx, D3D12MA::Allocator* allocator, uint32_t max_bones)
{
    if (!m_vertex_buffer)
        throw std::runtime_error("GpuMeshBuffer::enable_skinning: vertex buffer not uploaded yet");

    m_max_bones = max_bones;

    const uint64_t vb_size = static_cast<uint64_t>(m_vertex_count) * sizeof(Vertex);
    const uint64_t bone_palette_size = static_cast<uint64_t>(max_bones) * sizeof(float) * 16; // 4x4 matrix = 16 floats

    // ---- Create DEFAULT-heap skinned vertex buffer (UAV target) ----
    {
        D3D12MA::ALLOCATION_DESC alloc_desc{};
        alloc_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC buf_desc = CD3DX12_RESOURCE_DESC::Buffer(
            vb_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        throw_if_failed(
            allocator->CreateResource(
                &alloc_desc,
                &buf_desc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                &m_skinned_vertex_alloc,
                IID_PPV_ARGS(&m_skinned_vertex_buffer)),
            "D3D12MA: CreateResource (skinned vertex buffer) failed");

        m_skinned_vertex_buffer->SetName(L"GpuMeshBuffer::SkinnedVertexBuffer");

        // Seed the skinned buffer with the rest-pose vertex data so that fields
        // the shader never writes (UV, bone indices/weights) are correct from
        // the very first frame.
        copy_buffer(ctx, m_skinned_vertex_buffer, m_vertex_buffer, vb_size);
    }

    // ---- Create DEFAULT-heap bone palette buffer
    {
        D3D12MA::ALLOCATION_DESC alloc_desc{};
        alloc_desc.HeapType = D3D12_HEAP_TYPE_UPLOAD; // UPLOAD so we can map and update each frame

        D3D12_RESOURCE_DESC buf_desc = CD3DX12_RESOURCE_DESC::Buffer(bone_palette_size);

        throw_if_failed(
            allocator->CreateResource(
                &alloc_desc,
                &buf_desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                &m_bone_palette_alloc,
                IID_PPV_ARGS(&m_bone_palette_buffer)),
            "D3D12MA: CreateResource (bone palette buffer) failed");

        m_bone_palette_buffer->SetName(L"GpuMeshBuffer::BonePaletteBuffer");
    }

    // Persistently map the bone palette buffer so upload_bone_palette() needs no Map/Unmap.
    D3D12_RANGE read_range{ 0, 0 }; // CPU never reads back
    throw_if_failed(
        m_bone_palette_buffer->Map(0, &read_range, &m_bone_palette_mapped_ptr),
        "GpuMeshBuffer: failed to map bone palette buffer");

    // ---- Register skinned vertex buffer UAV in the bindless heap (ByteAddressBuffer UAV) ----
    m_skinned_vertex_uav_slot = ctx.allocate_bindless_slot();

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.Format              = DXGI_FORMAT_R32_TYPELESS;
    uav_desc.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.FirstElement = 0;
    uav_desc.Buffer.NumElements  = static_cast<UINT>(vb_size / 4); // count in DWORDs
    uav_desc.Buffer.StructureByteStride = 0;
    uav_desc.Buffer.Flags        = D3D12_BUFFER_UAV_FLAG_RAW;

    D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu_handle =
        ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
    uav_cpu_handle.ptr += static_cast<SIZE_T>(m_skinned_vertex_uav_slot) * ctx.bindless_descriptor_size();

    ctx.device()->CreateUnorderedAccessView(m_skinned_vertex_buffer, nullptr, &uav_desc, uav_cpu_handle);

    // ---- Register skinned vertex buffer SRV in the bindless heap (ByteAddressBuffer SRV) ----
    // This lets DXR hit shaders read the skinned output the same way they read the static buffer.
    m_skinned_vertex_srv_slot = ctx.allocate_bindless_slot();

    D3D12_SHADER_RESOURCE_VIEW_DESC skinned_srv_desc{};
    skinned_srv_desc.Format                     = DXGI_FORMAT_R32_TYPELESS;
    skinned_srv_desc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    skinned_srv_desc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    skinned_srv_desc.Buffer.FirstElement        = 0;
    skinned_srv_desc.Buffer.NumElements         = static_cast<UINT>(vb_size / 4);
    skinned_srv_desc.Buffer.StructureByteStride = 0;
    skinned_srv_desc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_RAW;

    D3D12_CPU_DESCRIPTOR_HANDLE skinned_srv_cpu =
        ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
    skinned_srv_cpu.ptr += static_cast<SIZE_T>(m_skinned_vertex_srv_slot) * ctx.bindless_descriptor_size();

    ctx.device()->CreateShaderResourceView(m_skinned_vertex_buffer, &skinned_srv_desc, skinned_srv_cpu);

    // ---- Register bone palette SRV in the bindless heap (StructuredBuffer<float4x4>) ----
    m_bone_palette_srv_slot = ctx.allocate_bindless_slot();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format                     = DXGI_FORMAT_UNKNOWN;
    srv_desc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srv_desc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Buffer.FirstElement        = 0;
    srv_desc.Buffer.NumElements         = max_bones;
    srv_desc.Buffer.StructureByteStride = sizeof(float) * 16; // 4x4 matrix
    srv_desc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle =
        ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
    srv_cpu_handle.ptr += static_cast<SIZE_T>(m_bone_palette_srv_slot) * ctx.bindless_descriptor_size();

    ctx.device()->CreateShaderResourceView(m_bone_palette_buffer, &srv_desc, srv_cpu_handle);
}

// ---------------------------------------------------------------------------
// GpuMeshBuffer::enable_wind_deform
// Allocates only the output vertex buffer (UAV + SRV) needed for GPU wind
// deformation. No bone palette is created. The output is exposed via the
// same skinned_vertex_uav_slot / skinned_vertex_srv_slot / skinned_vertex_buffer()
// accessors so dispatch_vegetation_wind() and refit_blas() can be used
// without any additional code paths.
// ---------------------------------------------------------------------------
void GpuMeshBuffer::enable_wind_deform(DeviceContext& ctx, D3D12MA::Allocator* allocator)
{
    if (!m_vertex_buffer)
        throw std::runtime_error("GpuMeshBuffer::enable_wind_deform: vertex buffer not uploaded yet");

    if (m_skinned_vertex_buffer)
        return; // already allocated (e.g. enable_skinning was called first)

    const uint64_t vb_size = static_cast<uint64_t>(m_vertex_count) * sizeof(Vertex);

    // ---- Create DEFAULT-heap output vertex buffer (UAV target) ----
    {
        D3D12MA::ALLOCATION_DESC alloc_desc{};
        alloc_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC buf_desc = CD3DX12_RESOURCE_DESC::Buffer(
            vb_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        throw_if_failed(
            allocator->CreateResource(
                &alloc_desc,
                &buf_desc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                &m_skinned_vertex_alloc,
                IID_PPV_ARGS(&m_skinned_vertex_buffer)),
            "D3D12MA: CreateResource (wind output vertex buffer) failed");

        m_skinned_vertex_buffer->SetName(L"GpuMeshBuffer::WindOutputVertexBuffer");

        // Seed with rest-pose data so the first frame before any wind dispatch
        // renders correctly.
        copy_buffer(ctx, m_skinned_vertex_buffer, m_vertex_buffer, vb_size);
    }

    // ---- Register UAV in the bindless heap (RWByteAddressBuffer) ----
    m_skinned_vertex_uav_slot = ctx.allocate_bindless_slot();
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
        uav_desc.Format              = DXGI_FORMAT_R32_TYPELESS;
        uav_desc.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
        uav_desc.Buffer.FirstElement = 0;
        uav_desc.Buffer.NumElements  = static_cast<UINT>(vb_size / 4);
        uav_desc.Buffer.Flags        = D3D12_BUFFER_UAV_FLAG_RAW;

        D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu =
            ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
        uav_cpu.ptr += static_cast<SIZE_T>(m_skinned_vertex_uav_slot) * ctx.bindless_descriptor_size();
        ctx.device()->CreateUnorderedAccessView(m_skinned_vertex_buffer, nullptr, &uav_desc, uav_cpu);
    }

    // ---- Register SRV in the bindless heap (ByteAddressBuffer) ----
    m_skinned_vertex_srv_slot = ctx.allocate_bindless_slot();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format                  = DXGI_FORMAT_R32_TYPELESS;
        srv_desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Buffer.FirstElement     = 0;
        srv_desc.Buffer.NumElements      = static_cast<UINT>(vb_size / 4);
        srv_desc.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_RAW;

        D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu =
            ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
        srv_cpu.ptr += static_cast<SIZE_T>(m_skinned_vertex_srv_slot) * ctx.bindless_descriptor_size();
        ctx.device()->CreateShaderResourceView(m_skinned_vertex_buffer, &srv_desc, srv_cpu);
    }

    // ---- Allocate compact float3 prev-position buffer (stride 12) ----
    // This holds last frame's deformed positions so the closest-hit shader can
    // compute per-vertex motion vectors for the denoiser / TAA reprojection.
    // The wind compute shader writes the current output position here before
    // overwriting it with the new deformed position.
    {
        const uint64_t prev_size = static_cast<uint64_t>(m_vertex_count) * sizeof(float) * 3;

        D3D12MA::ALLOCATION_DESC alloc_desc{};
        alloc_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC buf_desc = CD3DX12_RESOURCE_DESC::Buffer(
            prev_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        throw_if_failed(
            allocator->CreateResource(
                &alloc_desc,
                &buf_desc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                &m_wind_prev_pos_alloc,
                IID_PPV_ARGS(&m_wind_prev_pos_buffer)),
            "D3D12MA: CreateResource (wind prev-pos buffer) failed");
        m_wind_prev_pos_buffer->SetName(L"GpuMeshBuffer::WindPrevPosBuffer");

        // Seed with rest-pose positions (float3 extracted from Vertex) so the
        // very first frame gets zero object-motion vectors instead of garbage.
        // We copy via a staging buffer: read rest-pose float3 strides from the
        // vertex buffer is complex, so we just zero-fill — the denoiser handles
        // the first frame gracefully with near-zero object motion.
        // (The second frame onward the shader provides correct prev positions.)

        // UAV slot
        m_wind_prev_pos_uav_slot = ctx.allocate_bindless_slot();
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
            uav_desc.Format              = DXGI_FORMAT_R32_TYPELESS;
            uav_desc.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
            uav_desc.Buffer.FirstElement = 0;
            uav_desc.Buffer.NumElements  = static_cast<UINT>(prev_size / 4);
            uav_desc.Buffer.Flags        = D3D12_BUFFER_UAV_FLAG_RAW;

            D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu =
                ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
            uav_cpu.ptr += static_cast<SIZE_T>(m_wind_prev_pos_uav_slot) * ctx.bindless_descriptor_size();
            ctx.device()->CreateUnorderedAccessView(m_wind_prev_pos_buffer, nullptr, &uav_desc, uav_cpu);
        }

        // SRV slot
        m_wind_prev_pos_srv_slot = ctx.allocate_bindless_slot();
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
            srv_desc.Format                  = DXGI_FORMAT_R32_TYPELESS;
            srv_desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv_desc.Buffer.FirstElement     = 0;
            srv_desc.Buffer.NumElements      = static_cast<UINT>(prev_size / 4);
            srv_desc.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_RAW;

            D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu =
                ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
            srv_cpu.ptr += static_cast<SIZE_T>(m_wind_prev_pos_srv_slot) * ctx.bindless_descriptor_size();
            ctx.device()->CreateShaderResourceView(m_wind_prev_pos_buffer, &srv_desc, srv_cpu);
        }
    }
}

// ---------------------------------------------------------------------------
void GpuMeshBuffer::upload_bone_palette(const std::vector<Mat4x4>& bone_palette) const
{
    assert(m_bone_palette_mapped_ptr);
    assert(bone_palette.size() <= m_max_bones);

    const size_t copy_bytes = bone_palette.size() * sizeof(Mat4x4);
    std::memcpy(m_bone_palette_mapped_ptr, bone_palette.data(), copy_bytes);
}

// GpuMeshBuffer::destroy
// ---------------------------------------------------------------------------
void GpuMeshBuffer::destroy()
{
    if (m_vertex_buffer) { m_vertex_buffer->Release(); m_vertex_buffer = nullptr; }
    if (m_index_buffer)  { m_index_buffer->Release();  m_index_buffer  = nullptr; }
    if (m_skinned_vertex_buffer) { m_skinned_vertex_buffer->Release(); m_skinned_vertex_buffer = nullptr; }
    if (m_wind_prev_pos_buffer)  { m_wind_prev_pos_buffer->Release();  m_wind_prev_pos_buffer  = nullptr; }
    if (m_bone_palette_buffer)
    {
        if (m_bone_palette_mapped_ptr) { m_bone_palette_buffer->Unmap(0, nullptr); m_bone_palette_mapped_ptr = nullptr; }
        m_bone_palette_buffer->Release();
        m_bone_palette_buffer = nullptr;
    }
    if (m_vertex_alloc)          { m_vertex_alloc->Release();          m_vertex_alloc          = nullptr; }
    if (m_index_alloc)           { m_index_alloc->Release();           m_index_alloc           = nullptr; }
    if (m_skinned_vertex_alloc)  { m_skinned_vertex_alloc->Release();  m_skinned_vertex_alloc  = nullptr; }
    if (m_wind_prev_pos_alloc)   { m_wind_prev_pos_alloc->Release();   m_wind_prev_pos_alloc   = nullptr; }
    if (m_bone_palette_alloc)    { m_bone_palette_alloc->Release();    m_bone_palette_alloc    = nullptr; }
    m_vbv = {};
    m_ibv = {};
    m_vertex_count    = 0;
    m_index_count     = 0;
    m_vertex_srv_slot = UINT32_MAX;
    m_index_srv_slot  = UINT32_MAX;
    m_skinned_vertex_uav_slot  = UINT32_MAX;
    m_skinned_vertex_srv_slot  = UINT32_MAX;
    m_bone_palette_srv_slot    = UINT32_MAX;
    m_wind_prev_pos_uav_slot   = UINT32_MAX;
    m_wind_prev_pos_srv_slot   = UINT32_MAX;
    m_max_bones = 0;
    m_bone_palette_mapped_ptr = nullptr;
}

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------
GpuMeshBuffer::GpuMeshBuffer(GpuMeshBuffer&& o) noexcept
    : m_vertex_alloc(o.m_vertex_alloc)
    , m_index_alloc(o.m_index_alloc)
    , m_skinned_vertex_alloc(o.m_skinned_vertex_alloc)
    , m_bone_palette_alloc(o.m_bone_palette_alloc)
    , m_wind_prev_pos_alloc(o.m_wind_prev_pos_alloc)
    , m_vertex_buffer(o.m_vertex_buffer)
    , m_index_buffer(o.m_index_buffer)
    , m_skinned_vertex_buffer(o.m_skinned_vertex_buffer)
    , m_bone_palette_buffer(o.m_bone_palette_buffer)
    , m_wind_prev_pos_buffer(o.m_wind_prev_pos_buffer)
    , m_vbv(o.m_vbv)
    , m_ibv(o.m_ibv)
    , m_vertex_count(o.m_vertex_count)
    , m_index_count(o.m_index_count)
    , m_vertex_srv_slot(o.m_vertex_srv_slot)
    , m_index_srv_slot(o.m_index_srv_slot)
    , m_skinned_vertex_uav_slot(o.m_skinned_vertex_uav_slot)
    , m_skinned_vertex_srv_slot(o.m_skinned_vertex_srv_slot)
    , m_bone_palette_srv_slot(o.m_bone_palette_srv_slot)
    , m_wind_prev_pos_uav_slot(o.m_wind_prev_pos_uav_slot)
    , m_wind_prev_pos_srv_slot(o.m_wind_prev_pos_srv_slot)
    , m_max_bones(o.m_max_bones)
    , m_bone_palette_mapped_ptr(o.m_bone_palette_mapped_ptr)
    , m_bounds(o.m_bounds)
{
    o.m_vertex_alloc  = nullptr;
    o.m_index_alloc   = nullptr;
    o.m_skinned_vertex_alloc  = nullptr;
    o.m_bone_palette_alloc    = nullptr;
    o.m_wind_prev_pos_alloc   = nullptr;
    o.m_vertex_buffer = nullptr;
    o.m_index_buffer  = nullptr;
    o.m_skinned_vertex_buffer  = nullptr;
    o.m_bone_palette_buffer    = nullptr;
    o.m_wind_prev_pos_buffer   = nullptr;
    o.m_vertex_count  = 0;
    o.m_index_count   = 0;
    o.m_vertex_srv_slot = UINT32_MAX;
    o.m_index_srv_slot  = UINT32_MAX;
    o.m_skinned_vertex_uav_slot  = UINT32_MAX;
    o.m_skinned_vertex_srv_slot  = UINT32_MAX;
    o.m_bone_palette_srv_slot    = UINT32_MAX;
    o.m_wind_prev_pos_uav_slot   = UINT32_MAX;
    o.m_wind_prev_pos_srv_slot   = UINT32_MAX;
    o.m_max_bones = 0;
    o.m_bone_palette_mapped_ptr = nullptr;
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

// ---------------------------------------------------------------------------
// GpuMeshBuffer::create_cloth_mesh
// ---------------------------------------------------------------------------
void GpuMeshBuffer::create_cloth_mesh(DeviceContext&      ctx,
                                       D3D12MA::Allocator* allocator,
                                       uint32_t            grid_w,
                                       uint32_t            grid_h,
                                       float               rest_len)
{
    MeshData mesh;

    // Build vertices in the XZ plane (Y up).  UV (0,0) at top-left.
    const float inv_w = 1.0f / static_cast<float>(grid_w > 1 ? grid_w - 1 : 1);
    const float inv_h = 1.0f / static_cast<float>(grid_h > 1 ? grid_h - 1 : 1);
    for (uint32_t row = 0; row < grid_h; ++row)
    {
        for (uint32_t col = 0; col < grid_w; ++col)
        {
            Vertex v{};
            v.position = { col * rest_len, 0.0f, -(static_cast<float>(row) * rest_len) };
            v.normal   = { 0.0f, 1.0f, 0.0f };
            v.tangent  = { 1.0f, 0.0f, 0.0f };
            v.uv       = { col * inv_w, row * inv_h };
            // bone_indices / bone_weights left zero (cloth uses simulation positions)
            mesh.vertices.push_back(v);
        }
    }

    // Build quad indices (two triangles per quad)
    for (uint32_t row = 0; row < grid_h - 1; ++row)
    {
        for (uint32_t col = 0; col < grid_w - 1; ++col)
        {
            uint32_t tl = row * grid_w + col;
            uint32_t tr = tl + 1;
            uint32_t bl = tl + grid_w;
            uint32_t br = bl + 1;
            mesh.indices.insert(mesh.indices.end(), { tl, tr, bl, tr, br, bl });
        }
    }

    // Compute bounds
    Vec3 mn = mesh.vertices[0].position;
    Vec3 mx = mn;
    for (const auto& v : mesh.vertices)
    {
        mn.x = std::min(mn.x, v.position.x);
        mn.y = std::min(mn.y, v.position.y);
        mn.z = std::min(mn.z, v.position.z);
        mx.x = std::max(mx.x, v.position.x);
        mx.y = std::max(mx.y, v.position.y);
        mx.z = std::max(mx.z, v.position.z);
    }
    mesh.bounds.min_pt = mn;
    mesh.bounds.max_pt = mx;

    upload(ctx, allocator, mesh);
}

// ---------------------------------------------------------------------------
// ClothGpuResources helpers
// ---------------------------------------------------------------------------
static void create_float3_default_buffer(
    DeviceContext& ctx, D3D12MA::Allocator* allocator,
    uint32_t count, const wchar_t* name,
    D3D12MA::Allocation** out_alloc, ID3D12Resource** out_res,
    const float* initial_data = nullptr)
{
    const uint64_t byte_size = static_cast<uint64_t>(count) * 12u; // sizeof(float3)

    D3D12MA::ALLOCATION_DESC alloc_desc{};
    alloc_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC buf_desc = CD3DX12_RESOURCE_DESC::Buffer(
        byte_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    throw_if_failed(
        allocator->CreateResource(&alloc_desc, &buf_desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            out_alloc, IID_PPV_ARGS(out_res)),
        "ClothGpu: CreateResource (float3 buffer) failed");
    (*out_res)->SetName(name);

    if (initial_data)
        upload_buffer(ctx, allocator, *out_res, initial_data, byte_size);
}

static uint32_t register_float3_srv(DeviceContext& ctx, ID3D12Resource* res, uint32_t count)
{
    uint32_t slot = ctx.allocate_bindless_slot();
    const uint64_t byte_size = static_cast<uint64_t>(count) * 12u;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format                     = DXGI_FORMAT_R32_TYPELESS;
    srv_desc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srv_desc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Buffer.FirstElement        = 0;
    srv_desc.Buffer.NumElements         = static_cast<UINT>(byte_size / 4);
    srv_desc.Buffer.StructureByteStride = 0;
    srv_desc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_RAW;

    D3D12_CPU_DESCRIPTOR_HANDLE h = ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * ctx.bindless_descriptor_size();
    ctx.device()->CreateShaderResourceView(res, &srv_desc, h);
    return slot;
}

static uint32_t register_float3_uav(DeviceContext& ctx, ID3D12Resource* res, uint32_t count)
{
    uint32_t slot = ctx.allocate_bindless_slot();
    const uint64_t byte_size = static_cast<uint64_t>(count) * 12u;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.Format              = DXGI_FORMAT_R32_TYPELESS;
    uav_desc.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.FirstElement = 0;
    uav_desc.Buffer.NumElements  = static_cast<UINT>(byte_size / 4);
    uav_desc.Buffer.StructureByteStride = 0;
    uav_desc.Buffer.Flags        = D3D12_BUFFER_UAV_FLAG_RAW;

    D3D12_CPU_DESCRIPTOR_HANDLE h = ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * ctx.bindless_descriptor_size();
    ctx.device()->CreateUnorderedAccessView(res, nullptr, &uav_desc, h);
    return slot;
}

// ---------------------------------------------------------------------------
// ClothGpuResources::create
// ---------------------------------------------------------------------------
void ClothGpuResources::create(DeviceContext&          ctx,
                                D3D12MA::Allocator*     allocator,
                                uint32_t               vertex_count,
                                const std::vector<Vec3>& initial_positions)
{
    m_vertex_count = vertex_count;
    const uint32_t n = vertex_count;

    // Flatten initial positions to float array for upload
    std::vector<float> pos_flat(n * 3u);
    for (uint32_t i = 0; i < n && i < static_cast<uint32_t>(initial_positions.size()); ++i)
    {
        pos_flat[i * 3 + 0] = initial_positions[i].x;
        pos_flat[i * 3 + 1] = initial_positions[i].y;
        pos_flat[i * 3 + 2] = initial_positions[i].z;
    }

    // pos_curr — current positions (x_n), initialised from scene layout
    create_float3_default_buffer(ctx, allocator, n, L"Cloth::PosCurr",   &m_pos_curr_alloc,   &m_pos_curr,   pos_flat.data());
    // pos_prev — previous positions (x_{n-1}), initialised == pos_curr so
    //            implicit velocity v = (curr - prev)/dt starts at zero.
    create_float3_default_buffer(ctx, allocator, n, L"Cloth::PosPrev",   &m_pos_prev_alloc,   &m_pos_prev,   pos_flat.data());
    // pos_pred_a / pos_pred_b — scratch buffers for Jacobi CONSTRAIN
    create_float3_default_buffer(ctx, allocator, n, L"Cloth::PosPredA",  &m_pos_pred_a_alloc, &m_pos_pred_a);
    create_float3_default_buffer(ctx, allocator, n, L"Cloth::PosPredB",  &m_pos_pred_b_alloc, &m_pos_pred_b);

    // Register SRV and UAV for every buffer
    m_pos_prev_srv   = register_float3_srv(ctx, m_pos_prev,   n);
    m_pos_curr_srv   = register_float3_srv(ctx, m_pos_curr,   n);
    m_pos_pred_a_srv = register_float3_srv(ctx, m_pos_pred_a, n);
    m_pos_pred_b_srv = register_float3_srv(ctx, m_pos_pred_b, n);

    m_pos_prev_uav   = register_float3_uav(ctx, m_pos_prev,   n);
    m_pos_curr_uav   = register_float3_uav(ctx, m_pos_curr,   n);
    m_pos_pred_a_uav = register_float3_uav(ctx, m_pos_pred_a, n);
    m_pos_pred_b_uav = register_float3_uav(ctx, m_pos_pred_b, n);

    // Output vertex buffer (GpuVertex layout, 76 bytes per vertex)
    const uint64_t out_size = static_cast<uint64_t>(n) * 76u;
    {
        D3D12MA::ALLOCATION_DESC alloc_desc{};
        alloc_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC buf_desc = CD3DX12_RESOURCE_DESC::Buffer(
            out_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        throw_if_failed(
            allocator->CreateResource(&alloc_desc, &buf_desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr,
                &m_output_vtx_alloc, IID_PPV_ARGS(&m_output_vtx)),
            "ClothGpu: CreateResource (output vertex buffer) failed");
        m_output_vtx->SetName(L"Cloth::OutputVertexBuffer");
    }

    // UAV
    {
        m_output_vtx_uav = ctx.allocate_bindless_slot();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
        uav_desc.Format              = DXGI_FORMAT_R32_TYPELESS;
        uav_desc.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
        uav_desc.Buffer.FirstElement = 0;
        uav_desc.Buffer.NumElements  = static_cast<UINT>(out_size / 4);
        uav_desc.Buffer.StructureByteStride = 0;
        uav_desc.Buffer.Flags        = D3D12_BUFFER_UAV_FLAG_RAW;
        D3D12_CPU_DESCRIPTOR_HANDLE h = ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(m_output_vtx_uav) * ctx.bindless_descriptor_size();
        ctx.device()->CreateUnorderedAccessView(m_output_vtx, nullptr, &uav_desc, h);
    }

    // SRV
    {
        m_output_vtx_srv = ctx.allocate_bindless_slot();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format                     = DXGI_FORMAT_R32_TYPELESS;
        srv_desc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        srv_desc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Buffer.FirstElement        = 0;
        srv_desc.Buffer.NumElements         = static_cast<UINT>(out_size / 4);
        srv_desc.Buffer.StructureByteStride = 0;
        srv_desc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_RAW;
        D3D12_CPU_DESCRIPTOR_HANDLE h = ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(m_output_vtx_srv) * ctx.bindless_descriptor_size();
        ctx.device()->CreateShaderResourceView(m_output_vtx, &srv_desc, h);
    }
}

// ---------------------------------------------------------------------------
// ClothGpuResources::seed_output_from
// Copy the rest-pose vertex data (including UVs) from the mesh buffer into
// the output vertex buffer so the hit shader has valid UVs before the first
// cloth simulation dispatch.
// ---------------------------------------------------------------------------
void ClothGpuResources::seed_output_from(DeviceContext& ctx, const GpuMeshBuffer& mesh)
{
    const uint64_t byte_size = static_cast<uint64_t>(m_vertex_count) * 76u;
    copy_buffer(ctx, m_output_vtx, mesh.vertex_buffer_resource(), byte_size);
}

// ---------------------------------------------------------------------------
// ClothGpuResources::destroy
// ---------------------------------------------------------------------------
void ClothGpuResources::destroy()
{
    auto rel = [](D3D12MA::Allocation*& a, ID3D12Resource*& r)
    {
        if (r) { r->Release(); r = nullptr; }
        if (a) { a->Release(); a = nullptr; }
    };
    rel(m_pos_prev_alloc,   m_pos_prev);
    rel(m_pos_curr_alloc,   m_pos_curr);
    rel(m_pos_pred_a_alloc, m_pos_pred_a);
    rel(m_pos_pred_b_alloc, m_pos_pred_b);
    rel(m_output_vtx_alloc, m_output_vtx);
    m_pos_prev_srv   = m_pos_curr_srv   = UINT32_MAX;
    m_pos_pred_a_srv = m_pos_pred_b_srv = UINT32_MAX;
    m_pos_prev_uav   = m_pos_curr_uav   = UINT32_MAX;
    m_pos_pred_a_uav = m_pos_pred_b_uav = UINT32_MAX;
    m_output_vtx_uav = m_output_vtx_srv = UINT32_MAX;
    m_vertex_count = 0;
}

// ---------------------------------------------------------------------------
// ClothGpuResources — move semantics
// ---------------------------------------------------------------------------
ClothGpuResources::ClothGpuResources(ClothGpuResources&& o) noexcept
    : m_vertex_count(o.m_vertex_count)
    , m_pos_prev_alloc(o.m_pos_prev_alloc),   m_pos_curr_alloc(o.m_pos_curr_alloc)
    , m_pos_pred_a_alloc(o.m_pos_pred_a_alloc), m_pos_pred_b_alloc(o.m_pos_pred_b_alloc)
    , m_pos_prev(o.m_pos_prev), m_pos_curr(o.m_pos_curr)
    , m_pos_pred_a(o.m_pos_pred_a), m_pos_pred_b(o.m_pos_pred_b)
    , m_output_vtx_alloc(o.m_output_vtx_alloc), m_output_vtx(o.m_output_vtx)
    , m_pos_prev_srv(o.m_pos_prev_srv),     m_pos_curr_srv(o.m_pos_curr_srv)
    , m_pos_pred_a_srv(o.m_pos_pred_a_srv), m_pos_pred_b_srv(o.m_pos_pred_b_srv)
    , m_pos_prev_uav(o.m_pos_prev_uav),     m_pos_curr_uav(o.m_pos_curr_uav)
    , m_pos_pred_a_uav(o.m_pos_pred_a_uav), m_pos_pred_b_uav(o.m_pos_pred_b_uav)
    , m_output_vtx_uav(o.m_output_vtx_uav), m_output_vtx_srv(o.m_output_vtx_srv)
{
    o.m_vertex_count = 0;
    o.m_pos_prev_alloc   = o.m_pos_curr_alloc   = nullptr;
    o.m_pos_pred_a_alloc = o.m_pos_pred_b_alloc = nullptr;
    o.m_pos_prev   = o.m_pos_curr   = nullptr;
    o.m_pos_pred_a = o.m_pos_pred_b = nullptr;
    o.m_output_vtx_alloc = nullptr; o.m_output_vtx = nullptr;
    o.m_pos_prev_srv   = o.m_pos_curr_srv   = UINT32_MAX;
    o.m_pos_pred_a_srv = o.m_pos_pred_b_srv = UINT32_MAX;
    o.m_pos_prev_uav   = o.m_pos_curr_uav   = UINT32_MAX;
    o.m_pos_pred_a_uav = o.m_pos_pred_b_uav = UINT32_MAX;
    o.m_output_vtx_uav = o.m_output_vtx_srv = UINT32_MAX;
}

ClothGpuResources& ClothGpuResources::operator=(ClothGpuResources&& o) noexcept
{
    if (this != &o) { destroy(); new (this) ClothGpuResources(std::move(o)); }
    return *this;
}

// ===========================================================================
// EcosystemGpuResources
// ===========================================================================
namespace {

// Strides must mirror the HLSL structs in vegetation_placement.hlsl and
// vegetation_lod_selection.hlsl.
constexpr uint32_t k_vegetation_instance_stride = 64u; // VegetationInstanceGpu
constexpr uint32_t k_species_gpu_stride         = 16u; // SpeciesGpu (4 floats)

static uint32_t register_structured_uav(DeviceContext& ctx, ID3D12Resource* res,
                                        uint32_t num_elements, uint32_t stride)
{
    uint32_t slot = ctx.allocate_bindless_slot();
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.Format                     = DXGI_FORMAT_UNKNOWN;
    uav_desc.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.FirstElement        = 0;
    uav_desc.Buffer.NumElements         = num_elements;
    uav_desc.Buffer.StructureByteStride = stride;
    uav_desc.Buffer.Flags               = D3D12_BUFFER_UAV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE h = ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * ctx.bindless_descriptor_size();
    ctx.device()->CreateUnorderedAccessView(res, nullptr, &uav_desc, h);
    return slot;
}

static uint32_t register_structured_srv(DeviceContext& ctx, ID3D12Resource* res,
                                        uint32_t num_elements, uint32_t stride)
{
    uint32_t slot = ctx.allocate_bindless_slot();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format                     = DXGI_FORMAT_UNKNOWN;
    srv_desc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srv_desc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Buffer.FirstElement        = 0;
    srv_desc.Buffer.NumElements         = num_elements;
    srv_desc.Buffer.StructureByteStride = stride;
    srv_desc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE h = ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * ctx.bindless_descriptor_size();
    ctx.device()->CreateShaderResourceView(res, &srv_desc, h);
    return slot;
}

} // anonymous

void EcosystemGpuResources::create(DeviceContext&      ctx,
                                   D3D12MA::Allocator* allocator,
                                   uint32_t            max_instances,
                                   uint32_t            species_count,
                                   const float*        species_table)
{
    m_max_instances = max_instances;
    m_species_count = species_count;

    D3D12MA::ALLOCATION_DESC alloc_desc{};
    alloc_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    // ---- Instance buffer (RWStructuredBuffer<VegetationInstanceGpu>) -------
    {
        const uint64_t byte_size = static_cast<uint64_t>(max_instances) *
                                   k_vegetation_instance_stride;
        D3D12_RESOURCE_DESC buf_desc = CD3DX12_RESOURCE_DESC::Buffer(
            byte_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        throw_if_failed(
            allocator->CreateResource(&alloc_desc, &buf_desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr,
                &m_instance_alloc, IID_PPV_ARGS(&m_instance_buffer)),
            "Ecosystem: CreateResource (instance buffer) failed");
        m_instance_buffer->SetName(L"Ecosystem::InstanceBuffer");
        m_instance_uav = register_structured_uav(ctx, m_instance_buffer,
            max_instances, k_vegetation_instance_stride);
        m_instance_srv = register_structured_srv(ctx, m_instance_buffer,
            max_instances, k_vegetation_instance_stride);
    }

    // ---- Atomic counter (RWStructuredBuffer<uint>, single element) ---------
    {
        const uint64_t byte_size = 4u;
        D3D12_RESOURCE_DESC buf_desc = CD3DX12_RESOURCE_DESC::Buffer(
            byte_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        throw_if_failed(
            allocator->CreateResource(&alloc_desc, &buf_desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr,
                &m_counter_alloc, IID_PPV_ARGS(&m_counter_buffer)),
            "Ecosystem: CreateResource (counter buffer) failed");
        m_counter_buffer->SetName(L"Ecosystem::InstanceCounter");

        // Zero-initialise the counter via the copy queue.
        const uint32_t zero = 0u;
        upload_buffer(ctx, allocator, m_counter_buffer, &zero, byte_size);

        m_counter_uav = register_structured_uav(ctx, m_counter_buffer, 1u, 4u);
    }

    // ---- Readback buffers (CPU-visible) ------------------------------------
    {
        D3D12MA::ALLOCATION_DESC rb_alloc_desc{};
        rb_alloc_desc.HeapType = D3D12_HEAP_TYPE_READBACK;

        // Counter readback (4 bytes)
        {
            D3D12_RESOURCE_DESC buf_desc = CD3DX12_RESOURCE_DESC::Buffer(4u);
            throw_if_failed(
                allocator->CreateResource(&rb_alloc_desc, &buf_desc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    &m_counter_readback_alloc, IID_PPV_ARGS(&m_counter_readback)),
                "Ecosystem: CreateResource (counter readback) failed");
            m_counter_readback->SetName(L"Ecosystem::CounterReadback");
        }

        // Instance readback (full instance buffer size)
        {
            const uint64_t byte_size = static_cast<uint64_t>(max_instances) *
                                       k_vegetation_instance_stride;
            D3D12_RESOURCE_DESC buf_desc = CD3DX12_RESOURCE_DESC::Buffer(byte_size);
            throw_if_failed(
                allocator->CreateResource(&rb_alloc_desc, &buf_desc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    &m_instance_readback_alloc, IID_PPV_ARGS(&m_instance_readback)),
                "Ecosystem: CreateResource (instance readback) failed");
            m_instance_readback->SetName(L"Ecosystem::InstanceReadback");
        }
    }

    // ---- Species table (StructuredBuffer<SpeciesGpu>) ----------------------
    if (species_count > 0 && species_table)
    {
        const uint64_t byte_size = static_cast<uint64_t>(species_count) *
                                   k_species_gpu_stride;
        D3D12_RESOURCE_DESC buf_desc = CD3DX12_RESOURCE_DESC::Buffer(byte_size);
        throw_if_failed(
            allocator->CreateResource(&alloc_desc, &buf_desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr,
                &m_species_alloc, IID_PPV_ARGS(&m_species_buffer)),
            "Ecosystem: CreateResource (species buffer) failed");
        m_species_buffer->SetName(L"Ecosystem::SpeciesBuffer");

        upload_buffer(ctx, allocator, m_species_buffer, species_table, byte_size);

        m_species_srv = register_structured_srv(ctx, m_species_buffer,
            species_count, k_species_gpu_stride);
    }
}

void EcosystemGpuResources::destroy()
{
    auto rel = [](D3D12MA::Allocation*& a, ID3D12Resource*& r)
    {
        if (r) { r->Release(); r = nullptr; }
        if (a) { a->Release(); a = nullptr; }
    };
    rel(m_instance_alloc, m_instance_buffer);
    rel(m_counter_alloc,  m_counter_buffer);
    rel(m_species_alloc,  m_species_buffer);
    rel(m_instance_readback_alloc, m_instance_readback);
    rel(m_counter_readback_alloc,  m_counter_readback);
    m_instance_uav = m_instance_srv = UINT32_MAX;
    m_counter_uav  = m_species_srv  = UINT32_MAX;
    m_max_instances = 0;
    m_species_count = 0;
}

uint32_t EcosystemGpuResources::read_counter() const
{
    if (!m_counter_readback) return 0;
    uint32_t value = 0;
    D3D12_RANGE read_range{ 0, 4 };
    void* mapped = nullptr;
    if (SUCCEEDED(m_counter_readback->Map(0, &read_range, &mapped)) && mapped)
    {
        memcpy(&value, mapped, sizeof(value));
        D3D12_RANGE no_write{ 0, 0 };
        m_counter_readback->Unmap(0, &no_write);
    }
    return value;
}

uint32_t EcosystemGpuResources::read_instances(void* out, uint32_t count) const
{
    if (!m_instance_readback || !out || count == 0) return 0;
    const uint32_t safe_count = (count > m_max_instances) ? m_max_instances : count;
    const uint64_t byte_size  = static_cast<uint64_t>(safe_count) *
                                k_vegetation_instance_stride;
    D3D12_RANGE read_range{ 0, static_cast<SIZE_T>(byte_size) };
    void* mapped = nullptr;
    if (SUCCEEDED(m_instance_readback->Map(0, &read_range, &mapped)) && mapped)
    {
        memcpy(out, mapped, byte_size);
        D3D12_RANGE no_write{ 0, 0 };
        m_instance_readback->Unmap(0, &no_write);
    }
    return static_cast<uint32_t>(byte_size);
}

EcosystemGpuResources::EcosystemGpuResources(EcosystemGpuResources&& o) noexcept
    : m_max_instances(o.m_max_instances), m_species_count(o.m_species_count)
    , m_instance_alloc(o.m_instance_alloc), m_counter_alloc(o.m_counter_alloc)
    , m_species_alloc(o.m_species_alloc)
    , m_instance_readback_alloc(o.m_instance_readback_alloc)
    , m_counter_readback_alloc(o.m_counter_readback_alloc)
    , m_instance_buffer(o.m_instance_buffer), m_counter_buffer(o.m_counter_buffer)
    , m_species_buffer(o.m_species_buffer)
    , m_instance_readback(o.m_instance_readback)
    , m_counter_readback(o.m_counter_readback)
    , m_instance_uav(o.m_instance_uav), m_instance_srv(o.m_instance_srv)
    , m_counter_uav(o.m_counter_uav),   m_species_srv(o.m_species_srv)
{
    o.m_max_instances = 0;
    o.m_species_count = 0;
    o.m_instance_alloc = o.m_counter_alloc = o.m_species_alloc = nullptr;
    o.m_instance_readback_alloc = o.m_counter_readback_alloc = nullptr;
    o.m_instance_buffer = o.m_counter_buffer = o.m_species_buffer = nullptr;
    o.m_instance_readback = o.m_counter_readback = nullptr;
    o.m_instance_uav = o.m_instance_srv = UINT32_MAX;
    o.m_counter_uav  = o.m_species_srv  = UINT32_MAX;
}

EcosystemGpuResources& EcosystemGpuResources::operator=(EcosystemGpuResources&& o) noexcept
{
    if (this != &o) { destroy(); new (this) EcosystemGpuResources(std::move(o)); }
    return *this;
}

} // namespace mars
