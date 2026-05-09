// =============================================================================
// gpu_mesh_buffer.h
// MARS 3D Engine — GPU vertex / index buffer for a single mesh primitive
//
// Uploads CPU Vertex + Index data to D3D12 DEFAULT heap buffers and
// registers an SRV in the bindless heap so shaders can access vertices
// as a structured buffer (needed for DXR hit shaders and compute skinning).
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../d3d12_agility.h"
#include "asset_types.h"

#include <wrl/client.h>
#include <cstdint>

// Forward-declare D3D12MA types to avoid leaking the D3D12MemAlloc header.
namespace D3D12MA { class Allocator; class Allocation; }

namespace mars
{

using Microsoft::WRL::ComPtr;

class DeviceContext;

// =============================================================================
// GpuMeshBuffer
// =============================================================================
class MARS_ENGINE_API GpuMeshBuffer
{
public:
    GpuMeshBuffer()  = default;
    ~GpuMeshBuffer() { destroy(); }

    GpuMeshBuffer(const GpuMeshBuffer&)            = delete;
    GpuMeshBuffer& operator=(const GpuMeshBuffer&) = delete;

    GpuMeshBuffer(GpuMeshBuffer&&)            noexcept;
    GpuMeshBuffer& operator=(GpuMeshBuffer&&) noexcept;

    // Upload mesh data to the GPU.  Blocks until the copy queue is idle.
    // `allocator` is the D3D12MA allocator owned by ResourceManager.
    void upload(DeviceContext& ctx,
                D3D12MA::Allocator* allocator,
                const MeshData&     mesh);

    void destroy();

    // ---- Accessors ----------------------------------------------------------
    bool is_valid() const { return m_vertex_buffer != nullptr; }

    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view() const { return m_vbv; }
    D3D12_INDEX_BUFFER_VIEW  index_buffer_view()  const { return m_ibv; }

    uint32_t vertex_count()  const { return m_vertex_count; }
    uint32_t index_count()   const { return m_index_count; }

    // Bindless SRV slot for the vertex buffer (structured buffer, stride = sizeof(Vertex)).
    uint32_t vertex_srv_slot() const { return m_vertex_srv_slot; }

    const AABB& bounds() const { return m_bounds; }

private:
    D3D12MA::Allocation* m_vertex_alloc = nullptr;
    D3D12MA::Allocation* m_index_alloc  = nullptr;

    ID3D12Resource* m_vertex_buffer = nullptr;
    ID3D12Resource* m_index_buffer  = nullptr;

    D3D12_VERTEX_BUFFER_VIEW m_vbv = {};
    D3D12_INDEX_BUFFER_VIEW  m_ibv = {};

    uint32_t m_vertex_count    = 0;
    uint32_t m_index_count     = 0;
    uint32_t m_vertex_srv_slot = UINT32_MAX;

    AABB m_bounds = {};
};

} // namespace mars
