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
#include "../scene/scene_types.h"

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
    uint32_t material_index          = 0;
    uint32_t vertex_buffer_srv       = UINT32_MAX;
    uint32_t index_buffer_srv        = UINT32_MAX;
    uint32_t prev_vertex_buffer_srv  = UINT32_MAX; // previous-frame vertex positions (cloth); UINT32_MAX = none
    float    lod_alpha               = 1.0f;       // Stochastic LOD opacity in [0,1]; AnyHit rejects when rand >= lod_alpha
    uint32_t impostor_atlas_srv      = UINT32_MAX; // Octahedral impostor atlas (LOD3); UINT32_MAX = not an impostor
    uint32_t impostor_view_count     = 16;         // View-grid resolution per octahedral axis (e.g. 16 -> 16x16 atlas)
    uint32_t impostor_depth_normal_srv = UINT32_MAX; // Depth/normal atlas (RG=oct-normal, B=depth, A=roughness)
    float    impostor_aabb_min[3]    = {};         // Object-space AABB min (must match the BLAS procedural AABB)
    uint32_t _impostor_pad1          = 0;
    float    impostor_aabb_max[3]    = {};         // Object-space AABB max (must match the BLAS procedural AABB)
    uint32_t _impostor_pad2          = 0;
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
                        bool allow_update = false,
                        bool opaque = true);

    // Upload instance and material structured buffers and register their
    // bindless SRV slots so shaders can fetch per-instance/material data.
    // Must be called after all set_instance() calls and before the first trace().
    void upload_scene_buffers(DeviceContext& ctx,
                              const std::vector<CpuInstanceData>& instances,
                              const std::vector<CpuMaterialData>& materials);

    // Partial update: write `count` entries starting at `first_index` into the
    // already-allocated GPU instance buffer.  Call this after a LOD transition
    // to patch only the changed slots without reallocating the whole buffer.
    void upload_instance_data_range(DeviceContext& ctx,
                                    const CpuInstanceData* data,
                                    uint32_t first_index,
                                    uint32_t count);

    // ---------------------------------------------------------------------------
    // TLAS management
    // Rebuilds the top-level AS from the current instance list.
    // Call after modifying instances (add/remove/move).
    // ---------------------------------------------------------------------------
    void set_instance(uint32_t instance_index,
                      uint32_t blas_index,
                      const Mat4x4& transform,
                      uint32_t material_index);

    // Hide a TLAS instance from all ray types without removing it or aliasing
    // its BLAS. Safe to call every frame; takes effect at the next build_tlas().
    void hide_instance(uint32_t instance_index);

    void build_tlas(DeviceContext& ctx,
                    ID3D12GraphicsCommandList6* cmd_list,
                    uint32_t frame_index,
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
                                float sun_intensity,
                                const Mat4x4& prev_view_proj = Mat4x4::identity());

    // Set the sky mode used for ray misses.
    // Pass Type::HDRI and the bindless SRV slot of the equirectangular texture to enable HDRI sky.
    // Pass Type::Physical (or UINT32_MAX for hdri_slot) to revert to the procedural debug skybox.
    void set_sky(SkyboxDesc::Type type, uint32_t hdri_slot = UINT32_MAX);

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
                             DXGI_FORMAT back_buffer_format,
                             bool use_denoised = false);

    // Resize the UAV texture for a given output (e.g. on WM_SIZE).
    // new_width/new_height     — render resolution (DLSS internal, e.g. 853x480).
    // display_width/height     — display resolution written by DLSS into the
    //                           denoised output (e.g. 1280x720).  Pass 0 to use
    //                           the same dimensions as the render resolution.
    void resize_output(DeviceContext& ctx,
                       uint32_t output_index,
                       uint32_t new_width, uint32_t new_height,
                       uint32_t display_width = 0, uint32_t display_height = 0);

    // ---- Accessors ----------------------------------------------------------
    bool     is_initialised() const { return m_initialised; }
    uint32_t tlas_srv_slot()  const { return m_tlas_srv_slot; }

    // Returns the underlying D3D12 resource for the RGBA16F path-tracer UAV
    // for a given output (used by Denoiser for resource tagging).
    ID3D12Resource* output_resource(uint32_t output_index) const;

    // Returns the R16G16B16A16_FLOAT motion vector UAV resource for a given
    // output (used by Denoiser for resource tagging).
    ID3D12Resource* motion_vector_resource(uint32_t output_index) const;

    // Returns the R32F linear depth UAV resource for a given output.
    ID3D12Resource* depth_resource(uint32_t output_index) const;

    // Returns the RGBA16F denoised-output UAV resource for a given output.
    // This is a separate texture from output_resource(); it is used as the
    // write-back destination for DLSS-RR so that the noisy path-tracer UAV
    // and the denoised result never alias the same resource.
    ID3D12Resource* denoised_output_resource(uint32_t output_index) const;

    // Returns dedicated AOV UAV resources for DLSS-RR tagging.
    // These are separate textures; they start as black and will be written
    // by G-buffer shaders in M7/M8. DLSS-RR accepts the tags immediately,
    // which eliminates the kBufferTypeAlbedo missing-tag errors.
    ID3D12Resource* albedo_resource(uint32_t output_index)          const;
    ID3D12Resource* specular_albedo_resource(uint32_t output_index) const;
    ID3D12Resource* normals_aov_resource(uint32_t output_index)     const;
    ID3D12Resource* roughness_aov_resource(uint32_t output_index)   const;

    // Returns the GI reservoir structured buffer resource for a given output.
    // Element count = output_width * output_height * 2 (two ping-pong layers).
    ID3D12Resource* gi_reservoir_resource(uint32_t output_index) const;

    // Set the number of GI bounces (0 = off, 1 = single bounce, 2 = two bounces, …).
    // Default is 1. Set to 2 for richer color bleeding and inter-reflection.
    void set_gi_bounce_count(uint32_t count) { m_gi_bounce_count = count; }

    // ---------------------------------------------------------------------------
    // Animation / skinning
    // Build a BLAS that supports incremental refit (ALLOW_UPDATE flag).
    // The returned index should be used with refit_blas() each frame.
    // ---------------------------------------------------------------------------
    uint32_t build_skinned_blas(DeviceContext& ctx, const GpuMeshBuffer& mesh, bool opaque = true);

    // Refit a previously built skinned BLAS using the mesh's skinned vertex buffer
    // (populated by dispatch_skinning). Must be called before build_tlas().
    void refit_blas(DeviceContext& ctx,
                    ID3D12GraphicsCommandList6* cmd_list,
                    uint32_t blas_index,
                    const GpuMeshBuffer& mesh);

    // ---------------------------------------------------------------------------
    // Vegetation BLAS management
    // Build the refittable BLAS set for every vegetation species in the scene.
    // For each species:
    //   - LOD Near / Mid / FarCluster get an ALLOW_UPDATE BLAS so the wind
    //     compute pass can refit them per frame.
    //   - LOD Impostor uses a procedural AABB BLAS (single unit-cube AABB) so
    //     the path tracer can intersect it with a custom intersection shader.
    //
    // Fills out species.blas_indices[]. Assumes species.model_indices[] are
    // already populated by AssetImporter::import_vegetation_species() and that
    // the engine has uploaded GpuMeshBuffers for each LOD model.
    // ---------------------------------------------------------------------------
    uint32_t build_vegetation_lod_blas(DeviceContext& ctx,
                                       const GpuMeshBuffer& mesh,
                                       bool opaque = true);

    // Build a static BLAS over ALL submeshes of a vegetation LOD model in a
    // single acceleration structure. SpeedTree FBX assets consist of many
    // independently-placed submeshes (trunk / branches / leaves / fronds);
    // building only mesh_buffers.front() would render an incomplete tree.
    // This variant should be preferred when there is no wind compute pass to
    // refit a per-submesh skinned BLAS.
    uint32_t build_vegetation_model_blas(DeviceContext& ctx,
                                         const std::vector<GpuMeshBuffer>& meshes);

    // Build a procedural-AABB BLAS for the Impostor LOD (a single unit AABB
    // expanded to the species' bounding box). Returns the BLAS index.
    uint32_t build_vegetation_impostor_blas(DeviceContext& ctx,
                                            const Vec3& aabb_min,
                                            const Vec3& aabb_max);

    // Overload for cloth: refit using an explicit vertex resource (the cloth
    // output vertex buffer written by dispatch_cloth_sim).
    void refit_blas(DeviceContext& ctx,
                    ID3D12GraphicsCommandList6* cmd_list,
                    uint32_t blas_index,
                    ID3D12Resource* vertex_resource,
                    uint32_t vertex_count,
                    uint32_t index_count,
                    ID3D12Resource* index_resource);

    // Upload a bone palette (array of Mat4x4) to the mesh's bone palette buffer
    // and dispatch the skinning compute shader. Must be called before refit_blas().
    void dispatch_skinning(ID3D12GraphicsCommandList6* cmd_list,
                           const GpuMeshBuffer& mesh,
                           const std::vector<Mat4x4>& bone_palette);

    // ---------------------------------------------------------------------------
    // Cloth simulation
    // Dispatch the two-pass cloth compute shader (INTEGRATE then CONSTRAIN).
    // Must be called before refit_blas() for the cloth BLAS.
    // `ci` is the ClothInstance whose gpu buffers will be written.
    // ---------------------------------------------------------------------------
    void dispatch_cloth_sim(ID3D12GraphicsCommandList6* cmd_list,
                            ClothInstance&             ci,
                            float                      delta_time,
                            float                      time_seconds);

    // ---------------------------------------------------------------------------
    // Ecosystem / vegetation
    // Dispatch GPU-driven vegetation instance placement from a density map.
    // Populates the ecosystem GPU instance buffer and atomic counter.
    // Call once at scene load (or when density map changes).
    // ---------------------------------------------------------------------------
    void dispatch_vegetation_placement(ID3D12GraphicsCommandList6* cmd_list,
                                       EcosystemDesc&              ecosystem);

    // Dispatch GPU frustum and distance culling for all vegetation instances.
    // Marks instances outside the view frustum or beyond max_draw_distance as
    // LOD_CULLED. Call once per frame before dispatch_vegetation_lod_selection().
    void dispatch_vegetation_culling(ID3D12GraphicsCommandList6* cmd_list,
                                     EcosystemDesc&              ecosystem,
                                     const Vec3&                 camera_position,
                                     const Mat4x4&               view_proj);

    // Dispatch per-instance LOD selection. Reads the placed instance buffer and
    // the camera position, then writes the resulting VegetationLOD tier and a
    // stochastic dither value back into each instance for use by the path tracer.
    // Call once per frame after culling, before the path-trace dispatch.
    void dispatch_vegetation_lod_selection(ID3D12GraphicsCommandList6* cmd_list,
                                           EcosystemDesc&              ecosystem,
                                           const Vec3&                 camera_position,
                                           uint32_t                    frame_index);

    // Apply wind deformation to a single species mesh. Reads the rest-pose
    // vertex buffer (source_vertex_srv) and writes a deformed copy to
    // output_vertex_uav. Caller is responsible for issuing a UAV barrier and
    // calling refit_blas() on the species' refittable BLAS afterwards.
    void dispatch_vegetation_wind(ID3D12GraphicsCommandList6* cmd_list,
                                  uint32_t       vertex_count,
                                  uint32_t       source_vertex_srv,
                                  uint32_t       output_vertex_uav,
                                  uint32_t       prev_pos_uav,
                                  float          mesh_min_y,
                                  float          mesh_height,
                                  float          time_seconds,
                                  float          phase_offset,
                                  float          primary_bend_strength,
                                  float          primary_bend_speed,
                                  float          leaf_flutter_strength,
                                  float          leaf_flutter_speed,
                                  bool           is_leaf_mesh = false);

private:
    // --- Helpers -------------------------------------------------------------
    void create_allocator(DeviceContext& ctx);
    void create_rtpso(DeviceContext& ctx);
    void create_shader_tables(DeviceContext& ctx);
    void create_output_textures(DeviceContext& ctx);
    void create_cb_ring(DeviceContext& ctx);

    void create_blit_pipeline(DeviceContext& ctx);
    void create_skinning_pipeline(DeviceContext& ctx);
    void create_cloth_pipeline(DeviceContext& ctx);
    void create_vegetation_culling_pipeline(DeviceContext& ctx);
    void create_vegetation_placement_pipeline(DeviceContext& ctx);
    void create_vegetation_lod_pipeline(DeviceContext& ctx);
    void create_vegetation_wind_pipeline(DeviceContext& ctx);

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
        D3D12MA::Allocation* alloc          = nullptr;
        ID3D12Resource*      resource       = nullptr;
        // Only set for skinned (allow-update) BLASes — needed for refit each frame.
        D3D12MA::Allocation* scratch_alloc  = nullptr;
        ID3D12Resource*      scratch        = nullptr;
        bool                 allow_update     = false;
        bool                 geometry_opaque  = true;   // false for alpha-masked leaf/frond submeshes
        uint32_t             vertex_count     = 0;
        uint32_t             index_count      = 0;
        // Octahedral impostor procedural BLAS — routed to HitGroup_Impostor in the TLAS.
        bool                 is_impostor      = false;
    };
    std::vector<BlasEntry> m_blas_list;

    // --- TLAS ----------------------------------------------------------------
    D3D12MA::Allocation* m_tlas_alloc         = nullptr;
    ID3D12Resource*      m_tlas_resource      = nullptr;
    D3D12MA::Allocation* m_tlas_scratch_alloc = nullptr;
    ID3D12Resource*      m_tlas_scratch       = nullptr;
    D3D12MA::Allocation* m_instance_buf_alloc = nullptr;
    ID3D12Resource*      m_instance_buffer    = nullptr;  // D3D12_RAYTRACING_INSTANCE_DESC[]

    uint32_t m_tlas_srv_slot      = UINT32_MAX;

    struct InstanceDesc
    {
        uint32_t blas_index;
        uint32_t material_index;
        Mat4x4   transform;
        bool     hidden = false;  // InstanceMask=0 — invisible to all ray types
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

    // --- Per-output motion vector textures (R16G16B16A16_FLOAT) -------------
    std::vector<OutputTexture> m_mv_outputs;

    // --- Per-output linear depth textures (R32_FLOAT) -----------------------
    std::vector<OutputTexture> m_depth_outputs;

    // --- Per-output denoised output textures (RGBA16F) ----------------------
    // Separate from m_outputs so that DLSS-RR can read from m_outputs (noisy)
    // and write to m_denoised_outputs without aliasing.
    std::vector<OutputTexture> m_denoised_outputs;

    // --- Per-output AOV textures for DLSS-RR tagging ----------------------
    // Allocated at render resolution in UAV state. Black until M7/M8 G-buffer
    // shaders write to them. Satisfy kBufferTypeAlbedo / kBufferTypeSpecularAlbedo
    // / kBufferTypeNormals / kBufferTypeRoughness tags required by DLSS-RR.
    std::vector<OutputTexture> m_albedo_outputs;
    std::vector<OutputTexture> m_specular_albedo_outputs;
    std::vector<OutputTexture> m_normals_aov_outputs;
    std::vector<OutputTexture> m_roughness_aov_outputs;

    // --- Per-output GI reservoir structured buffers (M8) --------------------
    // Element count = width * height * 2  (ping-pong layers 0 and 1).
    // Written by path_trace.hlsl ClosestHit when gi_bounce_count > 0.
    struct GIBuffer
    {
        D3D12MA::Allocation* alloc    = nullptr;
        ID3D12Resource*      resource = nullptr;
        uint32_t             uav_slot = UINT32_MAX;
        uint32_t             width    = 0;
        uint32_t             height   = 0;
    };
    std::vector<GIBuffer> m_gi_reservoir_buffers;

    uint32_t m_gi_bounce_count = 1;  // passed to FrameConstants::gi_bounce_count each frame

    // --- Sky state -----------------------------------------------------------
    uint32_t m_sky_mode     = 0;          // 0 = procedural debug cube, 1 = HDRI
    uint32_t m_hdri_sky_slot = UINT32_MAX; // bindless SRV slot of the HDRI texture

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

    // --- Skinning compute pipeline -----------------------------------------
    ComPtr<ID3D12RootSignature> m_skinning_root_sig;
    ComPtr<ID3D12PipelineState> m_skinning_pso;

    // --- Cloth simulation compute pipeline --------------------------------
    ComPtr<ID3D12RootSignature> m_cloth_root_sig;
    ComPtr<ID3D12PipelineState> m_cloth_pso;

    // --- Vegetation frustum culling compute pipeline ---------------------
    ComPtr<ID3D12RootSignature> m_vegetation_culling_root_sig;
    ComPtr<ID3D12PipelineState> m_vegetation_culling_pso;

    // --- Vegetation placement compute pipeline ----------------------------
    ComPtr<ID3D12RootSignature> m_vegetation_placement_root_sig;
    ComPtr<ID3D12PipelineState> m_vegetation_placement_pso;

    // --- Vegetation LOD selection compute pipeline ------------------------
    ComPtr<ID3D12RootSignature> m_vegetation_lod_root_sig;
    ComPtr<ID3D12PipelineState> m_vegetation_lod_pso;

    // --- Vegetation wind deformation compute pipeline ---------------------
    ComPtr<ID3D12RootSignature> m_vegetation_wind_root_sig;
    ComPtr<ID3D12PipelineState> m_vegetation_wind_pso;

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
