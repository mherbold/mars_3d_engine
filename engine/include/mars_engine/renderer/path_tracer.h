// =============================================================================
// path_tracer.h
// MARS 3D Engine — DXR path-tracing pipeline
//
// PathTracer owns:
//   • The DXR ray-tracing pipeline state object (RTPSO)
//   • Shader tables (ray-gen, miss, hit-group records)
//   • Per-mesh BLAS acceleration structures
//   • The scene TLAS
//   • Per-output RGBA16F UAV textures (one per DisplayOutput)
//   • Per-frame upload constant buffer ring (FrameConstants)
//
// Usage (per frame):
//   1. Call begin_frame() to update FrameConstants and rebuild a dirty TLAS.
//   2. Call trace() to dispatch DispatchRays on the direct command queue.
//   3. Call copy_to_back_buffer() to blit the UAV into the swap-chain RT.
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../d3d12_agility.h"
#include "frame_constants.h"
#include "../asset/gpu_mesh_buffer.h"
#include "../math/math_types.h"

#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include <string>

// Forward-declare D3D12MA types to avoid leaking the D3D12MemAlloc header.
namespace D3D12MA { class Allocator; class Allocation; }

namespace mars
{

// ---------------------------------------------------------------------------
// CPU-side mirror of the HLSL GpuInstanceData / GpuMaterialData structs.
// Sizes and field order MUST match the HLSL definitions in material_data.hlsli.
// ---------------------------------------------------------------------------
struct CpuInstanceData
{
    float    world_transform[4][4]              = {};
    float    world_transform_inv_transpose[4][4]= {};
    uint32_t material_index    = 0;
    uint32_t vertex_buffer_srv = UINT32_MAX;
    uint32_t index_buffer_srv  = UINT32_MAX;
    uint32_t _pad              = 0;
};

struct CpuMaterialData
{
    float    base_color_factor[4]  = {1,1,1,1};
    float    metallic_factor       = 0.0f;
    float    roughness_factor      = 1.0f;
    float    emissive_scale        = 1.0f;
    float    alpha_cutoff          = 0.5f;
    uint32_t base_color_tex        = UINT32_MAX;
    uint32_t normal_tex            = UINT32_MAX;
    uint32_t metallic_roughness_tex= UINT32_MAX;
    uint32_t emissive_tex          = UINT32_MAX;
    float    emissive_factor[3]    = {};
    uint32_t flags                 = 0;  // bit0=double_sided, bit1=alpha_masked
};

using Microsoft::WRL::ComPtr;

class DeviceContext;
class GpuMeshBuffer;

// =============================================================================
// BlasBuildEntry — one entry per GpuMeshBuffer to build into a BLAS
// =============================================================================
struct BlasBuildEntry
{
    const GpuMeshBuffer* mesh    = nullptr;
    Mat4x4               transform = Mat4x4::identity(); // object-to-world (for instancing)
};

// =============================================================================
// PathTracer
// =============================================================================
class MARS_ENGINE_API PathTracer
{
public:
    PathTracer()  = default;
    ~PathTracer() { shutdown(); }

    PathTracer(const PathTracer&)            = delete;
    PathTracer& operator=(const PathTracer&) = delete;

    // ---------------------------------------------------------------------------
    // Initialise: compile the RTPSO from the embedded DXIL blob and allocate
    // per-frame resources.  `output_count` is the number of DisplayOutputs.
    // ---------------------------------------------------------------------------
    void init(DeviceContext& ctx,
              uint32_t output_width, uint32_t output_height,
              uint32_t output_count = 1);

    void shutdown();

    // ---------------------------------------------------------------------------
    // BLAS management
    // Build (or rebuild) a bottom-level AS for a single mesh.
    // Returns the BLAS index used when building the TLAS.
    // ---------------------------------------------------------------------------
    uint32_t build_blas(DeviceContext& ctx,
                        const GpuMeshBuffer& mesh,
                        bool allow_update = false);

    // Upload instance and material structured buffers and register their
    // bindless SRV slots so shaders can fetch per-instance/material data.
    // Must be called after all set_instance() calls and before the first trace().
    void upload_scene_buffers(DeviceContext& ctx,
                              const std::vector<CpuInstanceData>& instances,
                              const std::vector<CpuMaterialData>& materials);

    // ---------------------------------------------------------------------------
    // TLAS management
    // Rebuilds the top-level AS from the current instance list.
    // Call after modifying instances (add/remove/move).
    // ---------------------------------------------------------------------------
    void set_instance(uint32_t instance_index,
                      uint32_t blas_index,
                      const Mat4x4& transform,
                      uint32_t material_index);

    void build_tlas(DeviceContext& ctx,
                    ID3D12GraphicsCommandList6* cmd_list,
                    bool allow_update = false);

    // ---------------------------------------------------------------------------
    // Per-frame rendering
    // ---------------------------------------------------------------------------

    // Upload FrameConstants for the given output and frame index.
    void update_frame_constants(DeviceContext& ctx,
                                uint32_t output_index,
                                uint32_t frame_index,
                                const Vec3& camera_pos,
                                const Mat4x4& view_inv,
                                const Mat4x4& proj_inv,
                                const Vec3& sun_direction,
                                const Vec3& sun_color,
                                float sun_intensity);

    // Record DispatchRays into `cmd_list` for the given output.
    void trace(ID3D12GraphicsCommandList6* cmd_list,
               uint32_t output_index,
               uint32_t frame_index);

    // Blit the UAV for `output_index` into `back_buffer` (which must be in
    // RENDER_TARGET state; caller is responsible for transitions).
    // `rtv` is the CPU descriptor handle for the back-buffer RTV.
    // `hdr_mode` controls the tone-map / color-space path.
    // `back_buffer_format` must match the swap-chain format.
    void copy_to_back_buffer(ID3D12GraphicsCommandList6* cmd_list,
                             uint32_t output_index,
                             ID3D12Resource* back_buffer,
                             D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                             uint32_t hdr_mode,
                             DXGI_FORMAT back_buffer_format);

    // Resize the UAV texture for a given output (e.g. on WM_SIZE).
    void resize_output(DeviceContext& ctx,
                       uint32_t output_index,
                       uint32_t new_width, uint32_t new_height);

    // ---- Accessors ----------------------------------------------------------
    bool     is_initialised() const { return m_initialised; }
    uint32_t tlas_srv_slot()  const { return m_tlas_srv_slot; }

private:
    // --- Helpers -------------------------------------------------------------
    void create_allocator(DeviceContext& ctx);
    void create_rtpso(DeviceContext& ctx);
    void create_shader_tables(DeviceContext& ctx);
    void create_output_textures(DeviceContext& ctx);
    void create_cb_ring(DeviceContext& ctx);

    void create_blit_pipeline(DeviceContext& ctx);

    void release_output_textures();

    // Allocate a CPU-writable upload buffer and map it.
    void create_upload_buffer(DeviceContext& ctx,
                              uint64_t size, const wchar_t* name,
                              D3D12MA::Allocation** out_alloc,
                              ID3D12Resource** out_resource,
                              void** out_mapped_ptr);

    // --- RTPSO ---------------------------------------------------------------
    ComPtr<ID3D12StateObject>           m_rtpso;
    ComPtr<ID3D12StateObjectProperties> m_rtpso_props;
    ComPtr<ID3D12RootSignature>         m_global_root_sig;
    ComPtr<ID3D12RootSignature>         m_local_root_sig;   // empty local RS

    // --- Shader tables -------------------------------------------------------
    D3D12MA::Allocation* m_raygen_table_alloc  = nullptr;
    D3D12MA::Allocation* m_miss_table_alloc    = nullptr;
    D3D12MA::Allocation* m_hitgroup_table_alloc= nullptr;

    ID3D12Resource* m_raygen_table   = nullptr;
    ID3D12Resource* m_miss_table     = nullptr;
    ID3D12Resource* m_hitgroup_table = nullptr;

    static constexpr uint32_t k_shader_id_size       = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    static constexpr uint32_t k_raygen_record_stride  = k_shader_id_size; // no extra data
    static constexpr uint32_t k_miss_record_stride    = k_shader_id_size;
    static constexpr uint32_t k_hitgroup_record_stride= k_shader_id_size;

    // --- BLAS list -----------------------------------------------------------
    struct BlasEntry
    {
        D3D12MA::Allocation* alloc    = nullptr;
        ID3D12Resource*      resource = nullptr;
    };
    std::vector<BlasEntry> m_blas_list;

    // --- TLAS ----------------------------------------------------------------
    D3D12MA::Allocation* m_tlas_alloc         = nullptr;
    ID3D12Resource*      m_tlas_resource      = nullptr;
    D3D12MA::Allocation* m_tlas_scratch_alloc = nullptr;
    ID3D12Resource*      m_tlas_scratch       = nullptr;
    D3D12MA::Allocation* m_instance_buf_alloc = nullptr;
    ID3D12Resource*      m_instance_buffer    = nullptr;  // D3D12_RAYTRACING_INSTANCE_DESC[]

    uint32_t m_tlas_srv_slot = UINT32_MAX;

    struct InstanceDesc
    {
        uint32_t blas_index;
        uint32_t material_index;
        Mat4x4   transform;
    };
    std::vector<InstanceDesc> m_instances;

    // --- Per-output UAV textures (RGBA16F) -----------------------------------
    struct OutputTexture
    {
        D3D12MA::Allocation* alloc    = nullptr;
        ID3D12Resource*      resource = nullptr;
        uint32_t             uav_slot = UINT32_MAX;  // bindless UAV slot
        uint32_t             srv_slot = UINT32_MAX;  // bindless SRV slot (for tone-map blit)
        uint32_t             width    = 0;
        uint32_t             height   = 0;
    };
    std::vector<OutputTexture> m_outputs;

    // --- Per-frame constant buffer ring --------------------------------------
    // One upload buffer per output × k_frame_count slots.
    struct FrameCBSlot
    {
        D3D12MA::Allocation* alloc      = nullptr;
        ID3D12Resource*      resource   = nullptr;
        void*                mapped_ptr = nullptr;
        uint32_t             cbv_slot   = UINT32_MAX; // bindless CBV slot
    };
    // Indexed as m_frame_cbs[output_index * k_frame_count + back_index]
    std::vector<FrameCBSlot> m_frame_cbs;

    // --- Blit (tone-map) pipeline -------------------------------------------
    // One PSO per swap-chain format (SDR, HDR10, scRGB).
    ComPtr<ID3D12RootSignature> m_blit_root_sig;
    ComPtr<ID3D12PipelineState> m_blit_pso_sdr;    // DXGI_FORMAT_R8G8B8A8_UNORM
    ComPtr<ID3D12PipelineState> m_blit_pso_hdr10;  // DXGI_FORMAT_R10G10B10A2_UNORM
    ComPtr<ID3D12PipelineState> m_blit_pso_scrgb;  // DXGI_FORMAT_R16G16B16A16_FLOAT
    D3D12_GPU_DESCRIPTOR_HANDLE m_bindless_heap_gpu_start{}; // cached from DeviceContext

    // --- Scene instance / material structured buffers -----------------------
    D3D12MA::Allocation* m_instance_data_alloc   = nullptr;
    ID3D12Resource*      m_instance_data_buffer  = nullptr;
    uint32_t             m_instance_data_srv_slot= UINT32_MAX;

    D3D12MA::Allocation* m_material_data_alloc   = nullptr;
    ID3D12Resource*      m_material_data_buffer  = nullptr;
    uint32_t             m_material_data_srv_slot= UINT32_MAX;

    // --- D3D12MA allocator (owned by PathTracer) -----------------------------
    D3D12MA::Allocator* m_allocator = nullptr;

    uint32_t m_output_count  = 0;
    bool     m_initialised   = false;
};

} // namespace mars
