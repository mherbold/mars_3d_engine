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
#include "../math/math_types.h"

#include <wrl/client.h>
#include <cstdint>
#include <vector>

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

    // Enable skeletal animation for this mesh by allocating a skinned vertex buffer
    // and bone palette buffer. Must be called after upload().
    void enable_skinning(DeviceContext& ctx,
                        D3D12MA::Allocator* allocator,
                        uint32_t max_bones);

    void destroy();

    // ---- Accessors ----------------------------------------------------------
    bool is_valid() const { return m_vertex_buffer != nullptr; }
    bool is_skinned() const { return m_skinned_vertex_buffer != nullptr; }

    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view() const { return m_vbv; }
    D3D12_INDEX_BUFFER_VIEW  index_buffer_view()  const { return m_ibv; }

    // Raw D3D12 resources — needed for explicit BLAS geometry descriptors.
    ID3D12Resource* vertex_buffer_resource() const { return m_vertex_buffer; }
    ID3D12Resource* index_buffer_resource()  const { return m_index_buffer; }

    uint32_t vertex_count()  const { return m_vertex_count; }
    uint32_t index_count()   const { return m_index_count; }

    // Bindless SRV slot for the vertex buffer (structured buffer, stride = sizeof(Vertex)).
    uint32_t vertex_srv_slot() const { return m_vertex_srv_slot; }

    // Bindless SRV slot for the index buffer (ByteAddressBuffer).
    uint32_t index_srv_slot()  const { return m_index_srv_slot; }

    // Bindless UAV slot for the skinned vertex buffer (written by GPU skinning compute shader)
    uint32_t skinned_vertex_uav_slot() const { return m_skinned_vertex_uav_slot; }

    // Bindless SRV slot for the skinned vertex buffer (read by DXR hit shader after skinning).
    // Mirrors the same buffer as skinned_vertex_uav_slot but as a raw SRV.
    uint32_t skinned_vertex_srv_slot() const { return m_skinned_vertex_srv_slot; }

    // Bindless SRV slot for the bone palette structured buffer (array of 4x4 matrices)
    uint32_t bone_palette_srv_slot() const { return m_bone_palette_srv_slot; }

    // Get the skinned vertex buffer resource (for BLAS refit after skinning)
    ID3D12Resource* skinned_vertex_buffer() const { return m_skinned_vertex_buffer; }

    // Upload a bone palette (array of Mat4x4) directly into the mapped UPLOAD bone palette buffer.
    // bone_palette.size() must be <= max_bones passed to enable_skinning().
    void upload_bone_palette(const std::vector<Mat4x4>& bone_palette) const;

    const AABB& bounds() const { return m_bounds; }

    // Create a procedural cloth mesh (grid_w × grid_h regular grid) and upload it.
    // `rest_len` is the horizontal/vertical rest edge length in world units.
    // The grid is laid out in the XY plane (0,0)→(grid_w*rest_len, grid_h*rest_len).
    // Caller should then build a BLAS with allow_update=true for refit.
    void create_cloth_mesh(DeviceContext&      ctx,
                           D3D12MA::Allocator* allocator,
                           uint32_t            grid_w,
                           uint32_t            grid_h,
                           float               rest_len);

private:
    D3D12MA::Allocation* m_vertex_alloc = nullptr;
    D3D12MA::Allocation* m_index_alloc  = nullptr;
    D3D12MA::Allocation* m_skinned_vertex_alloc = nullptr;
    D3D12MA::Allocation* m_bone_palette_alloc   = nullptr;

    ID3D12Resource* m_vertex_buffer         = nullptr;
    ID3D12Resource* m_index_buffer          = nullptr;
    ID3D12Resource* m_skinned_vertex_buffer = nullptr;
    ID3D12Resource* m_bone_palette_buffer   = nullptr;

    D3D12_VERTEX_BUFFER_VIEW m_vbv = {};
    D3D12_INDEX_BUFFER_VIEW  m_ibv = {};

    uint32_t m_vertex_count    = 0;
    uint32_t m_index_count     = 0;
    uint32_t m_vertex_srv_slot = UINT32_MAX;
    uint32_t m_index_srv_slot  = UINT32_MAX;
    uint32_t m_skinned_vertex_uav_slot = UINT32_MAX;
    uint32_t m_skinned_vertex_srv_slot = UINT32_MAX;
    uint32_t m_bone_palette_srv_slot   = UINT32_MAX;
    uint32_t m_max_bones = 0;
    void*    m_bone_palette_mapped_ptr = nullptr;

    AABB m_bounds = {};
};

// =============================================================================
// ClothGpuResources — dynamic GPU buffers for one cloth simulation instance.
//
// Implements 3-buffer implicit-velocity XPBD (Verlet-style):
//   pos_prev  (x_{n-1}) — previous frame positions; implicit velocity = (curr - prev) / dt
//   pos_curr  (x_n)     — current frame positions; written by FINALIZE each substep
//   pos_pred_a / pos_pred_b — two scratch buffers Jacobi-ping-ponged during CONSTRAIN
//
// The four float3 buffers map to what were previously ping/pong position and
// velocity buffers.  No velocity buffers are needed; velocity is never stored.
//
// FINALIZE rotates state: prev ← curr, curr ← final_pred.
// No swap_buffers() call is needed between substeps.
// =============================================================================
class MARS_ENGINE_API ClothGpuResources
{
public:
    ClothGpuResources()  = default;
    ~ClothGpuResources() { destroy(); }

    ClothGpuResources(const ClothGpuResources&)            = delete;
    ClothGpuResources& operator=(const ClothGpuResources&) = delete;

    ClothGpuResources(ClothGpuResources&&)            noexcept;
    ClothGpuResources& operator=(ClothGpuResources&&) noexcept;

    // Allocate all buffers.  pos_prev is initialised equal to pos_curr so
    // implicit velocity starts at zero.
    void create(DeviceContext&          ctx,
                D3D12MA::Allocator*     allocator,
                uint32_t               vertex_count,
                const std::vector<Vec3>& initial_positions);

    // Copy the rest-pose vertex data (including UVs) from the given mesh buffer
    // into the output vertex buffer so the hit shader has valid UVs before the
    // first cloth simulation dispatch.
    void seed_output_from(DeviceContext& ctx, const GpuMeshBuffer& mesh);

    void destroy();

    bool is_valid() const { return m_pos_curr != nullptr; }

    // ---- Bindless slots -------------------------------------------------------
    // SRV slots (readable as ByteAddressBuffer float3[])
    uint32_t pos_prev_srv()    const { return m_pos_prev_srv; }
    uint32_t pos_curr_srv()    const { return m_pos_curr_srv; }
    uint32_t pos_pred_a_srv()  const { return m_pos_pred_a_srv; }
    uint32_t pos_pred_b_srv()  const { return m_pos_pred_b_srv; }
    // UAV slots (writable as RWByteAddressBuffer)
    uint32_t pos_prev_uav()    const { return m_pos_prev_uav; }
    uint32_t pos_curr_uav()    const { return m_pos_curr_uav; }
    uint32_t pos_pred_a_uav()  const { return m_pos_pred_a_uav; }
    uint32_t pos_pred_b_uav()  const { return m_pos_pred_b_uav; }
    // Output vertex buffer
    uint32_t output_vertex_uav() const { return m_output_vtx_uav; }
    uint32_t output_vertex_srv() const { return m_output_vtx_srv; }

    // Raw D3D12 resource for the output vertex buffer — used by refit_blas.
    ID3D12Resource* output_vertex_resource() const { return m_output_vtx; }

private:
    uint32_t m_vertex_count = 0;

    // 3-buffer positional state (all DEFAULT heap)
    D3D12MA::Allocation* m_pos_prev_alloc   = nullptr;
    D3D12MA::Allocation* m_pos_curr_alloc   = nullptr;
    D3D12MA::Allocation* m_pos_pred_a_alloc = nullptr;
    D3D12MA::Allocation* m_pos_pred_b_alloc = nullptr;
    ID3D12Resource*      m_pos_prev         = nullptr;
    ID3D12Resource*      m_pos_curr         = nullptr;
    ID3D12Resource*      m_pos_pred_a       = nullptr;
    ID3D12Resource*      m_pos_pred_b       = nullptr;

    // output vertex buffer (GpuVertex[] — same layout as skinning output)
    D3D12MA::Allocation* m_output_vtx_alloc = nullptr;
    ID3D12Resource*      m_output_vtx       = nullptr;

    // Bindless descriptor slots
    uint32_t m_pos_prev_srv    = UINT32_MAX;
    uint32_t m_pos_curr_srv    = UINT32_MAX;
    uint32_t m_pos_pred_a_srv  = UINT32_MAX;
    uint32_t m_pos_pred_b_srv  = UINT32_MAX;
    uint32_t m_pos_prev_uav    = UINT32_MAX;
    uint32_t m_pos_curr_uav    = UINT32_MAX;
    uint32_t m_pos_pred_a_uav  = UINT32_MAX;
    uint32_t m_pos_pred_b_uav  = UINT32_MAX;
    uint32_t m_output_vtx_uav  = UINT32_MAX;
    uint32_t m_output_vtx_srv  = UINT32_MAX;
};


} // namespace mars
