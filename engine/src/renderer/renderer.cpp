// =============================================================================
// renderer.cpp
// MARS 3D Engine — Renderer implementation (M5: scene loading + static scene)
// =============================================================================

#include "mars_engine/engine_api.h"
#include "mars_engine/renderer/renderer.h"
#include "mars_engine/scene/scene_loader.h"

#include <stdexcept>
#include <format>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <windows.h>
#include <wincodec.h>
#include <DirectXTex.h>

namespace mars
{

static void throw_if_failed(HRESULT hr, const char* msg)
{
    if (FAILED(hr))
        throw std::runtime_error(std::format("{} (HRESULT 0x{:08X})", msg, static_cast<unsigned>(hr)));
}

// CPU mirror of the GPU VegetationInstanceGpu struct (must match vegetation_placement.hlsl /
// vegetation_lod_selection.hlsl — 64 bytes, stride = k_vegetation_instance_stride).
struct VegetationInstanceGpu {
    float    position_scale[4];
    float    rotation[4];
    uint32_t species_index;
    uint32_t current_lod;
    uint32_t tlas_instance;
    float    wind_phase_offset;
    float    lod_dither;
    uint32_t _pad0;
    uint32_t _pad1;
    uint32_t _pad2;
};
static_assert(sizeof(VegetationInstanceGpu) == 64, "VegetationInstanceGpu must be 64 bytes");

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

    // Streamline must intercept DXGI/D3D12 creation — init before the device.
    Denoiser::pre_device_init();

    m_device_ctx.init();
    m_display_manager.init(m_device_ctx, configs, hwnds);
    m_resource_mgr.init(m_device_ctx);
    m_anim_system.initialize(&m_device_ctx, &m_resource_mgr);
    create_frame_resources();

    // Initialise PathTracer for the primary output dimensions.
    // Additional outputs get resize_output() calls when they are rendered.
    uint32_t output_count = m_display_manager.output_count();
    if (output_count > 0)
    {
        DisplayOutput& primary = m_display_manager.output(0);
        m_path_tracer.init(m_device_ctx,
                           primary.width(), primary.height(),
                           output_count);

        }

        // Initialise Denoiser (DLSS 4) — after path tracer so resources are known.
        // Denoiser must be initialised first so it can compute the DLSS render resolution.
        // The path tracer is then resized to that render resolution; its denoised output
        // texture is sized to the full display resolution (DLSS writes at display-res).
        m_denoiser.init(m_device_ctx, output_count);
        for (uint32_t i = 0; i < output_count; ++i)
        {
            DisplayOutput& disp = m_display_manager.output(i);
            uint32_t dw = disp.width(), dh = disp.height();
            m_denoiser.resize_output(i, dw, dh);

            uint32_t rw = dw, rh = dh;
            m_denoiser.get_render_resolution(i, rw, rh);
            // rw x rh  = render resolution  (DispatchRays, path-tracer UAVs)
            // dw x dh  = display resolution (denoised output that DLSS upscales into)
            m_path_tracer.resize_output(m_device_ctx, i, rw, rh, dw, dh);
        }

    // Initialise per-output camera state list
    m_cameras.resize(output_count);
    m_denoised_in_uav_state.assign(output_count, false);
    m_output_in_psr_state.assign(output_count, false);

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

    m_path_tracer.shutdown();
    m_denoiser.shutdown();
    m_anim_system.shutdown();
    m_scene.unload();
    // Release ecosystem GPU buffers BEFORE the resource manager tears down the
    // D3D12MA allocator. Otherwise these D3D12MA::Allocations outlive their
    // parent memory block and trip the
    // "Some allocations were not freed before destruction of this memory block!"
    // assertion inside D3D12MemAlloc on shutdown.
    m_ecosystem_gpu.clear();
    m_resource_mgr.shutdown();
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

    // Resize denoiser first so it can recompute the DLSS render resolution,
    // then resize the path tracer to that render resolution (not the display
    // resolution), so DispatchRays fills exactly the region DLSS will read.
    if (m_denoiser.is_initialised())
        m_denoiser.resize_output(output_index, width, height);

    uint32_t rw = width, rh = height;
    if (m_denoiser.is_initialised())
        m_denoiser.get_render_resolution(output_index, rw, rh);

    if (m_path_tracer.is_initialised())
        m_path_tracer.resize_output(m_device_ctx, output_index, rw, rh, width, height);

    // DenoisedOutputUAV is recreated in COMMON state on resize.
    if (output_index < m_denoised_in_uav_state.size())
        m_denoised_in_uav_state[output_index] = false;
}

void Renderer::on_resize(uint32_t width, uint32_t height)
{
    on_resize(0, width, height);
}

// ---------------------------------------------------------------------------
// set_camera
// ---------------------------------------------------------------------------
void Renderer::set_camera(uint32_t output_index,
                           const Vec3& position,
                           const Mat4x4& view_inv,
                           const Mat4x4& proj_inv)
{
    if (output_index >= m_cameras.size())
        m_cameras.resize(output_index + 1);

    CameraState& cam = m_cameras[output_index];

    // Compute proj * view (world→clip) — must match the shader convention mul(view_proj, pos).
    Mat4x4 curr_view = view_inv.inverse();
    Mat4x4 curr_proj = proj_inv.inverse();
    Mat4x4 curr_view_proj = curr_proj * curr_view;

    // Save previous-frame matrices before overwriting.
    // On the very first call (prev_view_proj is still zero), seed it with the
    // current VP so frame 0 produces zero motion vectors instead of garbage.
    bool is_first_frame = (cam.view_inv.m[0][0] == 0.0f && cam.view_inv.m[1][1] == 0.0f);
    if (is_first_frame)
    {
        cam.prev_view_proj = curr_view_proj;
        cam.prev_view_inv  = view_inv;
        cam.prev_proj_inv  = proj_inv;
    }
    else
    {
        Mat4x4 prev_view = cam.view_inv.inverse();
        Mat4x4 prev_proj = cam.proj_inv.inverse();
        cam.prev_view_proj = prev_proj * prev_view;
        cam.prev_view_inv  = cam.view_inv;
        cam.prev_proj_inv  = cam.proj_inv;
    }

    cam.position  = position;
    cam.view_inv  = view_inv;
    cam.proj_inv  = proj_inv;
    cam.view_proj = curr_view_proj;
}

// ---------------------------------------------------------------------------
// load_scene
// ---------------------------------------------------------------------------
bool Renderer::load_scene(const std::string& marsscene_path)
{
    if (!m_initialised)
    {
        MARS_LOG("[Renderer] load_scene() called before init().");
        return false;
    }

    m_device_ctx.flush_gpu();
    m_scene.unload();

    SceneLoader loader;
    if (!loader.load(marsscene_path, m_device_ctx, m_resource_mgr, m_scene))
        return false;


    // Build BLAS for every mesh in every model instance.
    // One BLAS per GpuMeshBuffer; share when the same model is used multiple times.
    std::vector<uint32_t> blas_base_index(m_resource_mgr.model_count(), UINT32_MAX);

    MARS_LOG("[Renderer] load_scene: {} scene instance(s), {} model(s) in resource manager.",
                 m_scene.instances().size(), m_resource_mgr.model_count());

    for (const auto& inst : m_scene.instances())
    {
        if (inst.model_index == UINT32_MAX) continue;

        // Skip if this model's BLAS has already been built (shared by multiple instances).
        if (blas_base_index[inst.model_index] != UINT32_MAX) continue;

        const GpuModel& model = m_resource_mgr.model(inst.model_index);
        MARS_LOG("[Renderer]   Building BLAS for model_index={} mesh_count={}",
                     inst.model_index, model.mesh_buffers.size());

        for (uint32_t mi = 0; mi < static_cast<uint32_t>(model.mesh_buffers.size()); ++mi)
        {
            MARS_LOG("[Renderer]     mesh[{}]: {} verts, {} idx",
                         mi,
                         model.mesh_buffers[mi].vertex_count(),
                         model.mesh_buffers[mi].index_count());

            uint32_t blas_idx;
            if (model.has_skeleton())
            {
                // Enable GPU skinning resources on first encounter.
                if (!model.mesh_buffers[mi].is_skinned())
                {
                    // GpuMeshBuffer is stored inside GpuModel via ResourceManager —
                    // cast away const to enable skinning (one-time initialisation).
                    GpuModel& mutable_model = m_resource_mgr.model(inst.model_index);
                    mutable_model.mesh_buffers[mi].enable_skinning(
                        m_device_ctx, m_resource_mgr.allocator(),
                        static_cast<uint32_t>(model.skeleton.bones.size()));
                }
                blas_idx = m_path_tracer.build_skinned_blas(m_device_ctx, model.mesh_buffers[mi]);
            }
            else
            {
                blas_idx = m_path_tracer.build_blas(m_device_ctx, model.mesh_buffers[mi]);
            }

            MARS_LOG("[Renderer]     -> BLAS index {} ({})", blas_idx,
                         model.has_skeleton() ? "skinned" : "static");

            if (mi == 0)
                blas_base_index[inst.model_index] = blas_idx;
        }
    }

    // Populate TLAS instances from scene instances.
    // Also build CpuInstanceData and CpuMaterialData arrays for the shader.
    uint32_t tlas_instance = 0;
    m_cpu_instances.clear();
    std::vector<CpuInstanceData>& cpu_instances = m_cpu_instances;
    std::vector<CpuMaterialData> cpu_materials;

    // One default white material (index 0)
    {
        CpuMaterialData def{};
        def.base_color_factor[0] = 0.8f;
        def.base_color_factor[1] = 0.8f;
        def.base_color_factor[2] = 0.8f;
        def.base_color_factor[3] = 1.0f;
        def.roughness_factor = 1.0f;
        cpu_materials.push_back(def);
    }

    for (uint32_t scene_inst_idx = 0;
         scene_inst_idx < m_scene.instance_count();
         ++scene_inst_idx)
    {
        SceneModelInstance& inst = const_cast<SceneModelInstance&>(
            m_scene.instances()[scene_inst_idx]);

        if (inst.model_index == UINT32_MAX) continue;

        const GpuModel& model = m_resource_mgr.model(inst.model_index);
        Mat4x4 world = inst.transform.to_matrix();

        // Create an AnimationState for this instance if the model has a skeleton
        // and the scene file requested animation for it.
        if (model.has_skeleton() && inst.anim_state_id == UINT32_MAX)
        {
            // Register skeleton and clips on first encounter.
            // (Idempotent: AnimationSystem checks for duplicates by content or ID.)
            uint32_t skel_id = m_anim_system.register_skeleton(model.skeleton);

            uint32_t initial_clip_id = 0;
            for (const auto& clip : model.animation_clips)
            {
                uint32_t cid = m_anim_system.register_animation_clip(clip);
                // Use the first clip, or the one named in the AnimationDesc.
                if (initial_clip_id == 0 || clip.name == inst.animation.clip_name)
                    initial_clip_id = cid;
            }

            inst.anim_state_id = m_anim_system.create_animation_state(skel_id, initial_clip_id);
            inst.skinned_blas_base = blas_base_index[inst.model_index];

            AnimationState* state = m_anim_system.get_state(inst.anim_state_id);
            if (state)
            {
                state->looping        = inst.animation.loop;
                state->playback_speed = inst.animation.speed;
            }

            MARS_LOG("[Renderer]   Instance '{}': anim_state={} skel_id={} clip={}",
                         inst.name, inst.anim_state_id, skel_id, initial_clip_id);
        }

        for (uint32_t mi = 0; mi < static_cast<uint32_t>(model.mesh_buffers.size()); ++mi)
        {
            uint32_t blas_idx = blas_base_index[inst.model_index] + mi;

            // Material: start from model's own texture slots, then apply any
            // per-instance override declared in the .marsscene file.
            uint32_t mat_idx = static_cast<uint32_t>(cpu_materials.size());
            {
                CpuMaterialData mat{};
                mat.base_color_factor[0] = 0.8f;
                mat.base_color_factor[1] = 0.8f;
                mat.base_color_factor[2] = 0.8f;
                mat.base_color_factor[3] = 1.0f;
                mat.roughness_factor = 0.8f;

                // Apply model's own base-colour texture (if any)
                if (mi < model.texture_slots.size() && model.texture_slots[mi] != UINT32_MAX)
                    mat.base_color_tex = model.texture_slots[mi];

                // Apply per-instance material override from the scene file
                const MaterialOverride& mo = inst.material_override;
                if (mo.has_any())
                {
                    auto load_override_tex = [&](const std::string& path, bool srgb) -> uint32_t
                    {
                        if (path.empty()) return UINT32_MAX;
                        return m_resource_mgr.load_texture(m_device_ctx, path, srgb);
                    };

                    uint32_t slot;
                    if (slot = load_override_tex(mo.base_color_texture, true);         slot != UINT32_MAX) mat.base_color_tex           = slot;
                    if (slot = load_override_tex(mo.normal_texture, false);             slot != UINT32_MAX) mat.normal_tex               = slot;
                    if (slot = load_override_tex(mo.metallic_roughness_texture, false); slot != UINT32_MAX) mat.metallic_roughness_tex   = slot;
                    if (slot = load_override_tex(mo.roughness_texture, false);          slot != UINT32_MAX) mat.metallic_roughness_tex   = slot;
                    if (slot = load_override_tex(mo.occlusion_texture, false);          slot != UINT32_MAX) mat.metallic_roughness_tex   = slot;
                    if (slot = load_override_tex(mo.emissive_texture, true);            slot != UINT32_MAX) mat.emissive_tex             = slot;

                    if (mo.base_color_r >= 0.0f) mat.base_color_factor[0] = mo.base_color_r;
                    if (mo.base_color_g >= 0.0f) mat.base_color_factor[1] = mo.base_color_g;
                    if (mo.base_color_b >= 0.0f) mat.base_color_factor[2] = mo.base_color_b;
                    if (mo.base_color_a >= 0.0f) mat.base_color_factor[3] = mo.base_color_a;
                    if (mo.metallic_factor  >= 0.0f) mat.metallic_factor  = mo.metallic_factor;
                    if (mo.roughness_factor >= 0.0f) mat.roughness_factor = mo.roughness_factor;
                    if (mo.emissive_scale   >= 0.0f) mat.emissive_scale   = mo.emissive_scale;
                }

                cpu_materials.push_back(mat);
            }

            // Instance record
            CpuInstanceData inst_data{};
            memcpy(inst_data.world_transform, world.m, sizeof(world.m));
            // For TRS with uniform scale the inverse-transpose upper-3x3 equals
            // the world upper-3x3.  Copy the same matrix; normals will be
            // re-normalised in the shader anyway.
            memcpy(inst_data.world_transform_inv_transpose, world.m, sizeof(world.m));
            inst_data.material_index    = mat_idx;
            // For skinned meshes the hit shader must read the skinned output buffer;
            // point vertex_buffer_srv at the skinned SRV slot (registered in enable_skinning).
            inst_data.vertex_buffer_srv = model.has_skeleton()
                ? model.mesh_buffers[mi].skinned_vertex_srv_slot()
                : model.mesh_buffers[mi].vertex_srv_slot();
            inst_data.index_buffer_srv  = model.mesh_buffers[mi].index_srv_slot();
            cpu_instances.push_back(inst_data);

            MARS_LOG("[Renderer]   TLAS instance {}: blas={} mat={} vtx_srv={} idx_srv={} ('{}')",
                         tlas_instance, blas_idx, mat_idx,
                         inst_data.vertex_buffer_srv, inst_data.index_buffer_srv,
                         inst.name);
            m_path_tracer.set_instance(tlas_instance++, blas_idx, world, mat_idx);
        }
    }

    // ---- Rigid-node animated instances ------------------------------------
    // Build a static BLAS per rigid node and create an AnimationState for
    // transform-only animations (wheels, doors, flags, etc.).
    for (auto& rn : m_scene.rigid_nodes())
    {
        if (rn.model_index == UINT32_MAX) continue;

        const GpuModel& model = m_resource_mgr.model(rn.model_index);
        if (rn.mesh_index >= static_cast<uint32_t>(model.mesh_buffers.size())) continue;

        // Static BLAS (no refit needed — transform change is handled by TLAS update).
        rn.blas_index = m_path_tracer.build_blas(m_device_ctx, model.mesh_buffers[rn.mesh_index]);

        // Create AnimationState if clips are present.
        if (!model.animation_clips.empty())
        {
            uint32_t skel_id = m_anim_system.register_skeleton(model.skeleton);
            uint32_t clip_id = 0;
            for (const auto& clip : model.animation_clips)
            {
                uint32_t cid = m_anim_system.register_animation_clip(clip);
                if (clip_id == 0 || clip.name == rn.clip_name)
                    clip_id = cid;
            }
            rn.anim_state_id = m_anim_system.create_animation_state(skel_id, clip_id);
            AnimationState* state = m_anim_system.get_state(rn.anim_state_id);
            if (state) { state->looping = rn.loop; state->playback_speed = rn.speed; }
        }

        // Register as a TLAS instance using its base transform.
        rn.current_world   = rn.base_transform.to_matrix();
        rn.tlas_instance   = tlas_instance;
        uint32_t mat_idx   = 0; // default material

        CpuInstanceData rn_data{};
        memcpy(rn_data.world_transform,                rn.current_world.m, sizeof(rn.current_world.m));
        memcpy(rn_data.world_transform_inv_transpose,  rn.current_world.m, sizeof(rn.current_world.m));
        rn_data.material_index    = mat_idx;
        rn_data.vertex_buffer_srv = model.mesh_buffers[rn.mesh_index].vertex_srv_slot();
        rn_data.index_buffer_srv  = model.mesh_buffers[rn.mesh_index].index_srv_slot();
        cpu_instances.push_back(rn_data);

        m_path_tracer.set_instance(tlas_instance++, rn.blas_index, rn.current_world, mat_idx);
        MARS_LOG("[Renderer]   Rigid node '{}': BLAS={} TLAS={}", rn.name, rn.blas_index, rn.tlas_instance);
    }

    // ---- Cloth simulation instances ----------------------------------------
    // Build a rest-pose cloth mesh, allocate ping-pong GPU buffers, and
    // create a refittable BLAS.  The cloth is simulated each frame by
    // dispatch_cloth_sim() and the BLAS is refitted before ray tracing.
    for (auto& ci : m_scene.cloth_instances())
    {
        const uint32_t gw = ci.cloth_desc.grid_w;
        const uint32_t gh = ci.cloth_desc.grid_h;
        const float    scale   = ci.base_transform.scale;
        const float    rl      = 1.0f;   // 1 unit per cell in local space; to_matrix() applies scale

        // Compute rest lengths: world-space spacing = scale * rl = scale
        ci.rest_len_struct = scale;
        ci.rest_len_shear  = scale * 1.41421356f;
        ci.rest_len_bend   = scale * 2.0f;

        // Build rest-pose mesh on GPU
        ci.mesh_buffer.create_cloth_mesh(m_device_ctx, m_resource_mgr.allocator(), gw, gh, rl);

        // Extract initial positions from the mesh vertex buffer for the simulation
        // buffers.  We regenerate from the grid formula (matches create_cloth_mesh).
        std::vector<Vec3> initial_positions;
        initial_positions.reserve(gw * gh);
        const Mat4x4 world = ci.base_transform.to_matrix();
        for (uint32_t row = 0; row < gh; ++row)
        {
            for (uint32_t col = 0; col < gw; ++col)
            {
                // Local space position from create_cloth_mesh (XZ plane)
                Vec3 local{ col * rl, 0.0f, -(static_cast<float>(row) * rl) };
                // Transform to world space
                Vec4 w4 = world.transform(Vec4{ local.x, local.y, local.z, 1.0f });
                initial_positions.push_back({ w4.x, w4.y, w4.z });
            }
        }

        ci.vertex_count = gw * gh;
        ci.index_count  = (gw - 1) * (gh - 1) * 6u;
        ci.gpu.create(m_device_ctx, m_resource_mgr.allocator(), ci.vertex_count, initial_positions);

        // Seed the cloth output vertex buffer with the rest-pose mesh data so
        // the hit shader has valid UVs before the first simulation dispatch.
        ci.gpu.seed_output_from(m_device_ctx, ci.mesh_buffer);

        // Enable skinning resources on the cloth mesh so build_skinned_blas has a
        // valid skinned vertex buffer to target.  Cloth uses no bones, so pass 1
        // as the minimum required by enable_skinning.
        if (!ci.mesh_buffer.is_skinned())
        {
            ci.mesh_buffer.enable_skinning(
                m_device_ctx, m_resource_mgr.allocator(), 1u);
        }

        // Build refittable BLAS using the rest-pose mesh (plain vertex buffer).
        // The cloth output vertex buffer (ci.gpu) is used for refits each frame.
        ci.cloth_blas_index = m_path_tracer.build_blas(m_device_ctx, ci.mesh_buffer, /*allow_update=*/true);

        // Material
        uint32_t mat_idx = static_cast<uint32_t>(cpu_materials.size());
        {
            CpuMaterialData mat{};
            mat.base_color_factor[0] = 0.9f;
            mat.base_color_factor[1] = 0.9f;
            mat.base_color_factor[2] = 0.9f;
            mat.base_color_factor[3] = 1.0f;
            mat.roughness_factor = 0.8f;

            // Apply per-instance material override from the scene file
            const MaterialOverride& mo = ci.cloth_desc.material;
            if (mo.has_any())
            {
                auto load_override_tex = [&](const std::string& path, bool srgb) -> uint32_t
                {
                    if (path.empty()) return UINT32_MAX;
                    return m_resource_mgr.load_texture(m_device_ctx, path, srgb);
                };

                uint32_t slot;
                if (slot = load_override_tex(mo.base_color_texture, true);         slot != UINT32_MAX) mat.base_color_tex           = slot;
                if (slot = load_override_tex(mo.normal_texture, false);             slot != UINT32_MAX) mat.normal_tex               = slot;
                if (slot = load_override_tex(mo.metallic_roughness_texture, false); slot != UINT32_MAX) mat.metallic_roughness_tex   = slot;
                if (slot = load_override_tex(mo.roughness_texture, false);          slot != UINT32_MAX) mat.metallic_roughness_tex   = slot;
                if (slot = load_override_tex(mo.occlusion_texture, false);          slot != UINT32_MAX) mat.metallic_roughness_tex   = slot;
                if (slot = load_override_tex(mo.emissive_texture, true);            slot != UINT32_MAX) mat.emissive_tex             = slot;

                if (mo.base_color_r >= 0.0f) mat.base_color_factor[0] = mo.base_color_r;
                if (mo.base_color_g >= 0.0f) mat.base_color_factor[1] = mo.base_color_g;
                if (mo.base_color_b >= 0.0f) mat.base_color_factor[2] = mo.base_color_b;
                if (mo.base_color_a >= 0.0f) mat.base_color_factor[3] = mo.base_color_a;
                if (mo.metallic_factor  >= 0.0f) mat.metallic_factor  = mo.metallic_factor;
                if (mo.roughness_factor >= 0.0f) mat.roughness_factor = mo.roughness_factor;
                if (mo.emissive_scale   >= 0.0f) mat.emissive_scale   = mo.emissive_scale;
            }

            cpu_materials.push_back(mat);
        }
        ci.mat_index = mat_idx;

        // Register TLAS instance
        ci.tlas_instance = tlas_instance;
        Mat4x4 identity  = Mat4x4::identity();
        CpuInstanceData cloth_data{};
        memcpy(cloth_data.world_transform,               identity.m, sizeof(identity.m));
        memcpy(cloth_data.world_transform_inv_transpose, identity.m, sizeof(identity.m));
        cloth_data.material_index        = mat_idx;
        cloth_data.vertex_buffer_srv     = ci.gpu.output_vertex_srv();
        cloth_data.index_buffer_srv      = ci.mesh_buffer.index_srv_slot();
        cloth_data.prev_vertex_buffer_srv= ci.gpu.pos_prev_srv();  // previous-frame positions for motion vectors
        cpu_instances.push_back(cloth_data);

        m_path_tracer.set_instance(tlas_instance++, ci.cloth_blas_index, identity, mat_idx);
        MARS_LOG("[Renderer]   Cloth '{}': {}x{} BLAS={} TLAS={}", ci.name, gw, gh, ci.cloth_blas_index, ci.tlas_instance);
    }

    MARS_LOG("[Renderer] Total TLAS instances to build: {}", tlas_instance);

    // M10: Bring up ecosystem GPU resources (species LOD BLASes, density
    // texture, etc.) *before* we build the TLAS so vegetation instances can
    // be included in the initial TLAS build.
    setup_ecosystem();

    // Build the TLAS and flush.
    throw_if_failed(m_cmd_allocators[0]->Reset(), "CmdAlloc::Reset (load_scene)");
    throw_if_failed(m_cmd_list->Reset(m_cmd_allocators[0].Get(), nullptr),
                    "CmdList::Reset (load_scene)");

    // M10: Run vegetation placement on the GPU, read back placed instances,
    // and append them to cpu_instances + reserve TLAS slots.
    place_and_register_vegetation(tlas_instance, cpu_instances, cpu_materials);

    rebuild_tlas();

    throw_if_failed(m_cmd_list->Close(), "CmdList::Close (load_scene)");
    ID3D12CommandList* lists[] = { m_cmd_list.Get() };
    m_device_ctx.direct_queue()->ExecuteCommandLists(1, lists);
    m_device_ctx.flush_gpu();

    // Upload per-instance and per-material structured buffers to the GPU.
    m_path_tracer.upload_scene_buffers(m_device_ctx, cpu_instances, cpu_materials);

    // Configure sky mode based on the scene's skybox descriptor.
    {
        const SkyboxDesc& sky = m_scene.skybox();
        if (sky.type == SkyboxDesc::Type::HDRI && !sky.hdri_path.empty())
        {
            // Load as linear (HDR environment maps must not be gamma-decoded)
            uint32_t hdri_slot = m_resource_mgr.load_texture(m_device_ctx, sky.hdri_path, /*is_srgb=*/false);
            m_path_tracer.set_sky(SkyboxDesc::Type::HDRI, hdri_slot);
            MARS_LOG("[Renderer] HDRI sky loaded: {} (slot {})", sky.hdri_path, hdri_slot);
        }
        else if (sky.type == SkyboxDesc::Type::Physical)
        {
            m_path_tracer.set_sky(SkyboxDesc::Type::Physical);
            MARS_LOG("[Renderer] Physical procedural sky active.");
        }
        else if (sky.type == SkyboxDesc::Type::Black)
        {
            m_path_tracer.set_sky(SkyboxDesc::Type::Black);
            MARS_LOG("[Renderer] Black sky active.");
        }
        else
        {
            m_path_tracer.set_sky(SkyboxDesc::Type::Debug);
            MARS_LOG("[Renderer] Procedural debug skybox active.");
        }
    }

    MARS_LOG("[Renderer] Scene '{}' loaded: {} TLAS instance(s).",
                 m_scene.name(), tlas_instance);
    return true;
}

// ---------------------------------------------------------------------------
// rebuild_tlas
// ---------------------------------------------------------------------------
void Renderer::rebuild_tlas()
{
    m_path_tracer.build_tlas(m_device_ctx, m_cmd_list.Get(), m_frame_index);
}

// ---------------------------------------------------------------------------
// setup_ecosystem (M10 step-10)
//
// Best-effort scene-load-time ecosystem bring-up. This:
//   - Loads each species' LOD models via ResourceManager::load_model (using
//     AssetImporter::import_vegetation_species to resolve SpeedTree ORCA v2
//     layouts).
//   - Builds a refittable BLAS for each LOD mesh and a procedural-AABB BLAS
//     for the Impostor LOD.
//   - Loads the density-map texture via ResourceManager::load_texture and
//     records its bindless SRV slot on the EcosystemDesc.
//
// GPU-buffer allocation for the instance/counter/species tables and TLAS
// instance reservation are intentionally not performed here — they require
// dedicated structured-buffer helpers that are not yet part of the engine's
// public API. The remaining setup paths will be filled in as those helpers
// land; until then `m_ecosystem_placed` stays false and dispatch_ecosystem()
// is a no-op.
// ---------------------------------------------------------------------------
void Renderer::setup_ecosystem()
{
    auto& ecosystems = m_scene.ecosystems();

    // Resize per-layer GPU state vectors to match the scene's ecosystem count.
    m_ecosystem_placed.assign(ecosystems.size(), false);
    m_ecosystem_dirty.assign(ecosystems.size(), false);
    m_ecosystem_gpu.resize(ecosystems.size());
    m_lod_readback_pending.assign(ecosystems.size(), false);

    namespace fs = std::filesystem;
    auto find_first_fbx = [](const fs::path& dir) -> std::string
    {
        if (!fs::exists(dir) || !fs::is_directory(dir)) return {};
        for (const auto& e : fs::directory_iterator(dir))
            if (e.is_regular_file() && e.path().extension() == ".fbx")
                return e.path().string();
        return {};
    };

    for (size_t layer_idx = 0; layer_idx < ecosystems.size(); ++layer_idx)
    {
        EcosystemDesc& eco = ecosystems[layer_idx];
        if (!eco.enabled || eco.species.empty())
            continue;

        MARS_LOG("[Renderer] setup_ecosystem[{}]: {} species, max_instances={}, density='{}'",
                 layer_idx, eco.species.size(), eco.max_instances, eco.density_map_path);

        // ---- Density map -----------------------------------------------------
        if (!eco.density_map_path.empty())
        {
            uint32_t slot = m_resource_mgr.load_texture(m_device_ctx, eco.density_map_path,
                                                        /*is_srgb=*/false);
            if (slot != UINT32_MAX)
                eco.density_map_srv = slot;
            else
                MARS_LOG("[Renderer] setup_ecosystem[{}]: failed to load density map '{}'",
                         layer_idx, eco.density_map_path);
        }

        // ---- Species assets + per-LOD BLAS -----------------------------------
        for (auto& sp : eco.species)
        {
            if (sp.asset_path.empty())
                continue;

            const fs::path base = sp.asset_path;
            const std::string high_fbx = find_first_fbx(base / "HighPoly");
            const std::string low_fbx  = find_first_fbx(base / "LowPoly");

            const std::string lod_paths[3] = { high_fbx, low_fbx, low_fbx };

            for (uint32_t li = 0; li < 3u; ++li)
            {
                if (lod_paths[li].empty()) continue;
                uint32_t mi = m_resource_mgr.load_model(m_device_ctx, lod_paths[li],
                                                        /*pre_transform_vertices=*/true);
                if (mi == UINT32_MAX) continue;
                sp.model_indices[li] = mi;

                const GpuModel& m = m_resource_mgr.model(mi);
                if (m.mesh_buffers.empty()) continue;

                sp.blas_indices[li].clear();
                sp.blas_indices[li].reserve(m.mesh_buffers.size());
                for (size_t mi_idx = 0; mi_idx < m.mesh_buffers.size(); ++mi_idx)
                {
                    auto& mb = m_resource_mgr.model(mi).mesh_buffers[mi_idx];
                    mb.enable_wind_deform(m_device_ctx, m_resource_mgr.allocator());

                    const uint32_t src_mat = (mi_idx < m.mesh_material_indices.size())
                                             ? m.mesh_material_indices[mi_idx] : 0u;
                    const bool alpha_masked = (src_mat < m.materials.size())
                                              && m.materials[src_mat].alpha_masked;
                    sp.blas_indices[li].push_back(
                        m_path_tracer.build_vegetation_lod_blas(m_device_ctx, mb,
                                                                 /*opaque=*/!alpha_masked));
                }
            }

            if (sp.model_indices[0] != UINT32_MAX)
            {
                const GpuModel& m0 = m_resource_mgr.model(sp.model_indices[0]);

                // The AABB passed to DXR is the *culling volume* — the intersection
                // shader only runs when a ray enters this box.  It must be large
                // enough to contain the full billboard square from every camera
                // direction: that square has half-extent = bounding_sphere_radius.
                // Using the tight mesh AABB here means rays that hit the billboard
                // outside the narrow tree silhouette never reach the shader, producing
                // a "cube with a clipping plane" artefact.  Use a sphere-cube instead.
                const Vec3& mn  = m0.bounds.min_pt;
                const Vec3& mx  = m0.bounds.max_pt;
                const Vec3  ctr { (mn.x + mx.x) * 0.5f,
                                  (mn.y + mx.y) * 0.5f,
                                  (mn.z + mx.z) * 0.5f };
                const float dx  = (mx.x - mn.x) * 0.5f;
                const float dy  = (mx.y - mn.y) * 0.5f;
                const float dz  = (mx.z - mn.z) * 0.5f;
                const float r   = std::sqrt(dx*dx + dy*dy + dz*dz);  // bounding sphere radius — matches baker

                const Vec3 sphere_min { ctr.x - r, ctr.y - r, ctr.z - r };
                const Vec3 sphere_max { ctr.x + r, ctr.y + r, ctr.z + r };

                const auto i_idx = static_cast<size_t>(VegetationLOD::Impostor);
                sp.blas_indices[i_idx].clear();
                sp.blas_indices[i_idx].push_back(
                    m_path_tracer.build_vegetation_impostor_blas(
                        m_device_ctx, sphere_min, sphere_max));
            }

            // ---- Impostor atlas texture ------------------------------------------
            // Look for impostor_atlas.dds (or impostor_atlas_debug.dds if debug mode
            // is enabled) in the species asset_path root.
            {
                const fs::path atlas_path = eco.impostor_debug_atlas
                                            ? base / "impostor_atlas_debug.dds"
                                            : base / "impostor_atlas.dds";
                if (fs::exists(atlas_path))
                {
                    uint32_t slot = m_resource_mgr.load_texture(m_device_ctx,
                                                                 atlas_path.string(),
                                                                 /*is_srgb=*/true);
                    if (slot != UINT32_MAX)
                    {
                        sp.impostor_atlas_srv = slot;
                        MARS_LOG("[Renderer]   Species '{}': impostor atlas loaded{} (srv={})",
                                 sp.name,
                                 eco.impostor_debug_atlas ? " [DEBUG]" : "",
                                 slot);
                    }
                    else
                    {
                        MARS_LOG("[Renderer]   Species '{}': WARNING — failed to load impostor atlas '{}'",
                                 sp.name, atlas_path.string());
                    }
                }
                else
                {
                    MARS_LOG("[Renderer]   Species '{}': impostor atlas not found at '{}' — impostor LOD will be invisible",
                             sp.name, atlas_path.string());
                }

                // Depth/normal atlas (only when using the real atlas, not debug).
                if (!eco.impostor_debug_atlas)
                {
                    const fs::path dn_atlas_path = base / "impostor_atlas_depth_normal.dds";
                    if (fs::exists(dn_atlas_path))
                    {
                        uint32_t dn_slot = m_resource_mgr.load_texture(m_device_ctx,
                                                                        dn_atlas_path.string(),
                                                                        /*is_srgb=*/false);
                        if (dn_slot != UINT32_MAX)
                        {
                            sp.impostor_depth_normal_srv = dn_slot;
                            MARS_LOG("[Renderer]   Species '{}': depth/normal atlas loaded (srv={})",
                                     sp.name, dn_slot);
                        }
                        else
                        {
                            MARS_LOG("[Renderer]   Species '{}': WARNING — failed to load depth/normal atlas '{}'",
                                     sp.name, dn_atlas_path.string());
                        }
                    }
                    else
                    {
                        MARS_LOG("[Renderer]   Species '{}': depth/normal atlas not found at '{}' — shading will use flat normal",
                                 sp.name, dn_atlas_path.string());
                    }
                }
            }

            MARS_LOG("[Renderer]   Species '{}': submeshes/LOD = [{},{},{},{}]",
                     sp.name,
                     sp.blas_indices[0].size(), sp.blas_indices[1].size(),
                     sp.blas_indices[2].size(), sp.blas_indices[3].size());
        }

        // ---- GPU resources ---------------------------------------------------
        // SpeciesGpu layout (32 bytes / 8 floats — must match vegetation_lod_selection.hlsl
        // and vegetation_culling.hlsl):
        //   [0] lod_near_max  [1] lod_mid_max  [2] lod_far_max  [3] max_draw_distance
        //   [4] bounding_radius  [5..7] _pad
        const uint32_t species_count = static_cast<uint32_t>(eco.species.size());
        std::vector<float> species_table(static_cast<size_t>(species_count) * 8u);
        for (uint32_t i = 0; i < species_count; ++i)
        {
            species_table[i * 8 + 0] = eco.species[i].lod_near_max;
            species_table[i * 8 + 1] = eco.species[i].lod_mid_max;
            species_table[i * 8 + 2] = eco.species[i].lod_far_max;
            species_table[i * 8 + 3] = eco.species[i].max_draw_distance;
            species_table[i * 8 + 4] = eco.species[i].bounding_radius;
            species_table[i * 8 + 5] = 0.0f; // _pad
            species_table[i * 8 + 6] = 0.0f; // _pad
            species_table[i * 8 + 7] = 0.0f; // _pad
        }

        m_ecosystem_gpu[layer_idx].destroy();
        m_ecosystem_gpu[layer_idx].create(m_device_ctx,
                                          m_resource_mgr.allocator(),
                                          eco.max_instances,
                                          species_count,
                                          species_table.data());

        eco.instance_buffer_uav  = m_ecosystem_gpu[layer_idx].instance_uav();
        eco.instance_counter_uav = m_ecosystem_gpu[layer_idx].counter_uav();
        eco.species_buffer_srv   = m_ecosystem_gpu[layer_idx].species_srv();

        MARS_LOG("[Renderer] setup_ecosystem[{}]: GPU buffers ready (instance_uav={}, counter_uav={}, species_srv={})",
                 layer_idx, eco.instance_buffer_uav, eco.instance_counter_uav, eco.species_buffer_srv);
    }
}

// ---------------------------------------------------------------------------
// place_and_register_vegetation (M10)
//
// Synchronously runs the GPU placement compute shader, reads back the atomic
// instance counter and the produced VegetationInstanceGpu records, then
// appends each placed instance as a TLAS instance and a CpuInstanceData entry.
// Must be invoked between m_cmd_list reset and rebuild_tlas() in load_scene().
// ---------------------------------------------------------------------------
void Renderer::place_and_register_vegetation(uint32_t&                          tlas_instance,
                                             std::vector<CpuInstanceData>&      cpu_instances,
                                             std::vector<CpuMaterialData>&      cpu_materials)
{
    auto& ecosystems = m_scene.ecosystems();

    auto ensure_species_materials = [&](SpeciesDesc& sp)
    {
        for (size_t lod = 0; lod < static_cast<size_t>(VegetationLOD::Count); ++lod)
        {
            if (!sp.material_indices[lod].empty()) continue;
            const uint32_t mi = sp.model_indices[lod];
            if (mi == UINT32_MAX) continue;
            const GpuModel& gm = m_resource_mgr.model(mi);
            sp.material_indices[lod].resize(gm.mesh_buffers.size(), 0u);

            for (size_t s = 0; s < gm.mesh_buffers.size(); ++s)
            {
                CpuMaterialData mat{};
                mat.base_color_factor[0] = 1.0f;
                mat.base_color_factor[1] = 1.0f;
                mat.base_color_factor[2] = 1.0f;
                mat.base_color_factor[3] = 1.0f;
                mat.roughness_factor     = 1.0f;
                mat.alpha_cutoff         = 0.5f;
                mat.flags = 0x1u; // bit0=double_sided

                const uint32_t src_mat = (s < gm.mesh_material_indices.size())
                                         ? gm.mesh_material_indices[s] : 0u;
                if (src_mat < gm.texture_slots.size())
                    mat.base_color_tex = gm.texture_slots[src_mat];
                if (src_mat < gm.normal_slots.size())
                    mat.normal_tex = gm.normal_slots[src_mat];
                if (src_mat < gm.mr_slots.size())
                    mat.metallic_roughness_tex = gm.mr_slots[src_mat];

                if (src_mat < gm.materials.size())
                {
                    const auto& src = gm.materials[src_mat];
                    mat.base_color_factor[0] = src.base_color_factor.x;
                    mat.base_color_factor[1] = src.base_color_factor.y;
                    mat.base_color_factor[2] = src.base_color_factor.z;
                    mat.base_color_factor[3] = src.base_color_factor.w;
                    mat.metallic_factor      = src.metallic_factor;
                    mat.roughness_factor     = src.roughness_factor;
                    mat.alpha_cutoff         = src.alpha_cutoff;
                    if (src.double_sided) mat.flags |= 0x1u;
                    if (src.alpha_masked) mat.flags |= 0x2u;
                }

                sp.material_indices[lod][s] = static_cast<uint32_t>(cpu_materials.size());
                cpu_materials.push_back(mat);
            }
        }
    };

    for (size_t layer_idx = 0; layer_idx < ecosystems.size(); ++layer_idx)
    {
        EcosystemDesc& eco = ecosystems[layer_idx];
        if (!eco.enabled || eco.species.empty())
            continue;

        if (eco.instance_buffer_uav  == UINT32_MAX ||
            eco.instance_counter_uav == UINT32_MAX ||
            eco.species_buffer_srv   == UINT32_MAX ||
            eco.density_map_srv      == UINT32_MAX)
        {
            MARS_LOG("[Renderer] place_and_register_vegetation[{}]: resources not ready, skipping", layer_idx);
            continue;
        }

        EcosystemGpuResources& gpu = m_ecosystem_gpu[layer_idx];
        ID3D12Resource* counter_default  = gpu.counter_resource();
        ID3D12Resource* instance_default = gpu.instance_resource();
        ID3D12Resource* counter_readback = gpu.counter_readback();
        ID3D12Resource* instance_readback= gpu.instance_readback();
        if (!counter_default || !instance_default || !counter_readback || !instance_readback)
            continue;

        ID3D12DescriptorHeap* heaps[] = { m_device_ctx.bindless_heap() };
        m_cmd_list->SetDescriptorHeaps(1, heaps);

        D3D12_RESOURCE_BARRIER pre[2]{};
        pre[0] = CD3DX12_RESOURCE_BARRIER::Transition(counter_default,
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pre[1] = CD3DX12_RESOURCE_BARRIER::Transition(instance_default,
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_cmd_list->ResourceBarrier(2, pre);

        m_path_tracer.dispatch_vegetation_placement(m_cmd_list.Get(), eco);

        D3D12_RESOURCE_BARRIER post[2]{};
        post[0] = CD3DX12_RESOURCE_BARRIER::Transition(counter_default,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        post[1] = CD3DX12_RESOURCE_BARRIER::Transition(instance_default,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_cmd_list->ResourceBarrier(2, post);

        m_cmd_list->CopyResource(counter_readback, counter_default);

        const uint64_t instance_bytes = static_cast<uint64_t>(eco.max_instances) * 64ull;
        m_cmd_list->CopyBufferRegion(instance_readback, 0, instance_default, 0, instance_bytes);

        D3D12_RESOURCE_BARRIER end_barriers[2]{};
        end_barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(counter_default,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        end_barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(instance_default,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        m_cmd_list->ResourceBarrier(2, end_barriers);

        throw_if_failed(m_cmd_list->Close(), "CmdList::Close (place_vegetation)");
        ID3D12CommandList* lists[] = { m_cmd_list.Get() };
        m_device_ctx.direct_queue()->ExecuteCommandLists(1, lists);
        m_device_ctx.flush_gpu();

        throw_if_failed(m_cmd_allocators[0]->Reset(), "CmdAlloc::Reset (place_vegetation)");
        throw_if_failed(m_cmd_list->Reset(m_cmd_allocators[0].Get(), nullptr),
                        "CmdList::Reset (place_vegetation)");

        const uint32_t placed_count_raw = gpu.read_counter();
        const uint32_t placed_count     = std::min(placed_count_raw, eco.max_instances);
        eco.instance_count = placed_count;

        MARS_LOG("[Renderer] place_and_register_vegetation[{}]: GPU reported {} placed instances (clamped to {})",
                 layer_idx, placed_count_raw, placed_count);

        if (placed_count == 0)
        {
            m_ecosystem_placed[layer_idx] = true;
            m_ecosystem_dirty[layer_idx]  = false;
            continue;
        }

        std::vector<VegetationInstanceGpu> instances(placed_count);
        gpu.read_instances(instances.data(), placed_count);

        eco.instances.clear();
        eco.instances.reserve(placed_count);

        for (uint32_t i = 0; i < placed_count; ++i)
        {
            const VegetationInstanceGpu& g = instances[i];
            if (g.species_index >= eco.species.size())
                continue;

            SpeciesDesc& sp = eco.species[g.species_index];
            ensure_species_materials(sp);

            const auto lod_idx = static_cast<size_t>(VegetationLOD::Near);
            if (sp.blas_indices[lod_idx].empty())
                continue;
            if (sp.model_indices[lod_idx] == UINT32_MAX)
                continue;

            const Vec3       pos   { g.position_scale[0], g.position_scale[1], g.position_scale[2] };
            const float      scale = g.position_scale[3];
            const Quaternion rot   { g.rotation[0], g.rotation[1], g.rotation[2], g.rotation[3] };

            Transform xf;
            xf.position = pos;
            xf.rotation = rot;
            xf.scale    = scale;
            const Mat4x4 world = xf.to_matrix();

            const GpuModel& m0 = m_resource_mgr.model(sp.model_indices[lod_idx]);
            const size_t submesh_count = sp.blas_indices[lod_idx].size();
            uint32_t first_tlas_for_inst = tlas_instance;
            for (size_t s = 0; s < submesh_count; ++s)
            {
                const uint32_t blas_idx = sp.blas_indices[lod_idx][s];
                if (blas_idx == UINT32_MAX) continue;
                if (s >= m0.mesh_buffers.size()) break;

                const uint32_t mat_idx = (s < sp.material_indices[lod_idx].size())
                                         ? sp.material_indices[lod_idx][s] : 0u;

                CpuInstanceData inst_data{};
                memcpy(inst_data.world_transform,               world.m, sizeof(world.m));
                memcpy(inst_data.world_transform_inv_transpose, world.m, sizeof(world.m));
                inst_data.material_index    = mat_idx;
                inst_data.vertex_buffer_srv = (m0.mesh_buffers[s].skinned_vertex_srv_slot() != UINT32_MAX)
                                              ? m0.mesh_buffers[s].skinned_vertex_srv_slot()
                                              : m0.mesh_buffers[s].vertex_srv_slot();
                inst_data.index_buffer_srv  = m0.mesh_buffers[s].index_srv_slot();
                inst_data.prev_vertex_buffer_srv = m0.mesh_buffers[s].wind_prev_pos_srv_slot();
                // Impostor fields — used by the intersection/closest-hit shaders
                // when this instance transitions to LOD3 (Impostor). Set here so
                // the initial upload_scene_buffers() call already has valid values.
                inst_data.impostor_atlas_srv        = sp.impostor_atlas_srv;
                inst_data.impostor_depth_normal_srv = sp.impostor_depth_normal_srv;
                inst_data.impostor_view_count  = 16u;
                {
                    // Store the sphere-cube AABB that matches the BLAS built above.
                    // The intersection shader uses these to derive the billboard center
                    // and half-extent; they must match the BLAS extents so every ray
                    // that enters the BLAS AABB also enters the billboard card.
                    const Vec3& mn  = m0.bounds.min_pt;
                    const Vec3& mx  = m0.bounds.max_pt;
                    const float cx  = (mn.x + mx.x) * 0.5f;
                    const float cy  = (mn.y + mx.y) * 0.5f;
                    const float cz  = (mn.z + mx.z) * 0.5f;
                    const float hdx = (mx.x - mn.x) * 0.5f;
                    const float hdy = (mx.y - mn.y) * 0.5f;
                    const float hdz = (mx.z - mn.z) * 0.5f;
                    const float r   = std::sqrt(hdx*hdx + hdy*hdy + hdz*hdz);
                    inst_data.impostor_aabb_min[0] = cx - r;
                    inst_data.impostor_aabb_min[1] = cy - r;
                    inst_data.impostor_aabb_min[2] = cz - r;
                    inst_data.impostor_aabb_max[0] = cx + r;
                    inst_data.impostor_aabb_max[1] = cy + r;
                    inst_data.impostor_aabb_max[2] = cz + r;
                }
                cpu_instances.push_back(inst_data);

                m_path_tracer.set_instance(tlas_instance, blas_idx, world, mat_idx);
                ++tlas_instance;
            }

            VegetationInstance vi{};
            vi.transform         = xf;
            vi.species_index     = g.species_index;
            vi.current_lod       = VegetationLOD::Near;
            vi.tlas_instance     = first_tlas_for_inst;
            vi.tlas_slot_count   = static_cast<uint32_t>(tlas_instance - first_tlas_for_inst);
            vi.wind_phase_offset = g.wind_phase_offset;
            eco.instances.push_back(vi);
        }

        m_ecosystem_placed[layer_idx] = true;
        m_ecosystem_dirty[layer_idx]  = false;

        MARS_LOG("[Renderer] place_and_register_vegetation[{}]: registered {} vegetation TLAS instances (total now {})",
                 layer_idx, eco.instances.size(), tlas_instance);
    }

    (void)cpu_materials;
}

// ---------------------------------------------------------------------------
// dispatch_ecosystem (M10 step-10)
//
// Per-frame ecosystem GPU work. Runs vegetation placement once (when
// instance/counter/species GPU resources are valid), frustum culling every
// frame to mark out-of-view instances, then LOD selection on visible instances.
// ---------------------------------------------------------------------------
void Renderer::dispatch_ecosystem()
{
    auto& ecosystems = m_scene.ecosystems();

    for (size_t layer_idx = 0; layer_idx < ecosystems.size(); ++layer_idx)
    {
        EcosystemDesc& eco = ecosystems[layer_idx];
        if (!eco.enabled || eco.species.empty())
            continue;

        if (eco.instance_buffer_uav  == UINT32_MAX ||
            eco.instance_counter_uav == UINT32_MAX ||
            eco.species_buffer_srv   == UINT32_MAX ||
            eco.density_map_srv      == UINT32_MAX)
        {
            continue;
        }

        if (layer_idx < m_ecosystem_placed.size() && !m_ecosystem_placed[layer_idx])
        {
            m_ecosystem_placed[layer_idx] = true;
            m_ecosystem_dirty[layer_idx]  = true;
        }

        if (eco.instance_count > 0 && !m_cameras.empty())
        {
            const CameraState& cam = m_cameras[0];

            // Frustum + distance culling — marks LOD_CULLED on invisible instances
            m_path_tracer.dispatch_vegetation_culling(
                m_cmd_list.Get(), eco, cam.position, cam.view_proj);

            // UAV barrier: culling writes must be visible to LOD selection
            D3D12_RESOURCE_BARRIER cull_uav{};
            cull_uav.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            cull_uav.UAV.pResource = nullptr; // all UAVs
            m_cmd_list->ResourceBarrier(1, &cull_uav);

            // LOD selection — assigns Near/Mid/FarCluster/Impostor to visible instances
            m_path_tracer.dispatch_vegetation_lod_selection(
                m_cmd_list.Get(), eco, cam.position, m_frame_index);

            // Schedule readback of updated LOD values. A UAV→COPY_SOURCE
            // transition is needed so the copy engine can read the UAV buffer.
            // The actual CPU read happens at the start of the NEXT frame after
            // wait_for_frame() confirms the GPU has finished this work.
            if (layer_idx < m_ecosystem_gpu.size() &&
                m_ecosystem_gpu[layer_idx].instance_resource() &&
                m_ecosystem_gpu[layer_idx].instance_readback())
            {
                ID3D12Resource* src = m_ecosystem_gpu[layer_idx].instance_resource();
                const uint64_t  bytes = static_cast<uint64_t>(eco.instance_count) *
                                        sizeof(VegetationInstanceGpu);

                D3D12_RESOURCE_BARRIER to_copy =
                    CD3DX12_RESOURCE_BARRIER::Transition(src,
                        D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
                m_cmd_list->ResourceBarrier(1, &to_copy);

                m_cmd_list->CopyBufferRegion(
                    m_ecosystem_gpu[layer_idx].instance_readback(), 0,
                    src, 0, bytes);

                D3D12_RESOURCE_BARRIER to_common =
                    CD3DX12_RESOURCE_BARRIER::Transition(src,
                        D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_COMMON);
                m_cmd_list->ResourceBarrier(1, &to_common);

                if (layer_idx < m_lod_readback_pending.size())
                    m_lod_readback_pending[layer_idx] = true;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// apply_vegetation_lod_updates
//
// Reads back the per-instance current_lod values written by the previous
// frame's LOD selection compute pass and updates each vegetation instance's
// TLAS entry to reference the correct per-LOD BLAS.
//
// Must be called AFTER wait_for_frame() so the GPU has finished writing the
// readback buffer.  LOD_CULLED (0xFFFFFFFF) instances are hidden by pointing
// them at the same BLAS but with a degenerate (zero-scale) transform would be
// ideal; for now we simply keep them at their last valid LOD so culled
// instances are merely frozen in their previous LOD tier.
// ---------------------------------------------------------------------------
void Renderer::apply_vegetation_lod_updates()
{
    auto& ecosystems = m_scene.ecosystems();

    for (size_t layer_idx = 0; layer_idx < ecosystems.size(); ++layer_idx)
    {
        if (layer_idx >= m_lod_readback_pending.size() ||
            !m_lod_readback_pending[layer_idx])
            continue;

        m_lod_readback_pending[layer_idx] = false;

        EcosystemDesc& eco = ecosystems[layer_idx];
        if (eco.instances.empty() || eco.instance_count == 0)
            continue;

        const uint32_t read_count = std::min(eco.instance_count,
            static_cast<uint32_t>(eco.instances.size()));

        std::vector<VegetationInstanceGpu> gpu_instances(read_count);
        m_ecosystem_gpu[layer_idx].read_instances(gpu_instances.data(), read_count);

        for (uint32_t i = 0; i < read_count; ++i)
        {
            const VegetationInstanceGpu& gi = gpu_instances[i];
            VegetationInstance&          vi = eco.instances[i];

            constexpr uint32_t LOD_CULLED = 0xFFFFFFFFu;
            if (gi.current_lod == LOD_CULLED)
                continue; // keep previous LOD BLAS until instance becomes visible again

            const auto new_lod = static_cast<VegetationLOD>(
                std::min(gi.current_lod,
                         static_cast<uint32_t>(VegetationLOD::Impostor)));

            if (new_lod == vi.current_lod)
                continue; // no change — skip redundant set_instance calls

            const uint32_t sp_idx = vi.species_index;
            if (sp_idx >= eco.species.size()) continue;
            const SpeciesDesc& sp = eco.species[sp_idx];

            const auto li = static_cast<size_t>(new_lod);
            if (sp.blas_indices[li].empty()) continue;
            // Note: sp.model_indices[li] is UINT32_MAX for the Impostor LOD (procedural
            // AABB — no model). Only skip if this is a mesh LOD with no model loaded.
            const bool is_impostor_lod = (new_lod == VegetationLOD::Impostor);
            if (!is_impostor_lod && sp.model_indices[li] == UINT32_MAX) continue;

            const size_t submesh_n = sp.blas_indices[li].size(); // 1 for impostor, N for mesh LODs
            const Mat4x4 world = vi.transform.to_matrix();

            // Update only the slots used by the new LOD.
            for (size_t s = 0; s < submesh_n; ++s)
            {
                const uint32_t blas_idx  = sp.blas_indices[li][s];
                if (blas_idx == UINT32_MAX) continue;

                const uint32_t tlas_slot = vi.tlas_instance + static_cast<uint32_t>(s);

                const uint32_t mat_idx = (s < sp.material_indices[li].size())
                                         ? sp.material_indices[li][s] : 0u;

                m_path_tracer.set_instance(tlas_slot, blas_idx, world, mat_idx);

                // Also patch the persistent CPU instance mirror so the GPU instance
                // buffer reflects the new LOD's vertex/index/material data.
                // Without this, shaders keep reading HighPoly mesh buffers even after
                // the TLAS has been switched to a LowPoly or Impostor BLAS.
                if (tlas_slot < m_cpu_instances.size())
                {
                    CpuInstanceData& id = m_cpu_instances[tlas_slot];
                    id.material_index = mat_idx;

                    if (is_impostor_lod)
                    {
                        // Impostor: no mesh geometry — clear mesh buffer slots so any
                        // errant mesh-path code reads UINT32_MAX rather than stale data.
                        id.vertex_buffer_srv      = UINT32_MAX;
                        id.index_buffer_srv       = UINT32_MAX;
                        id.prev_vertex_buffer_srv = UINT32_MAX;
                    }
                    else if (sp.model_indices[li] != UINT32_MAX)
                    {
                        const GpuModel& lod_model = m_resource_mgr.model(sp.model_indices[li]);
                        if (s < lod_model.mesh_buffers.size())
                        {
                            const auto& mb = lod_model.mesh_buffers[s];
                            id.vertex_buffer_srv = (mb.skinned_vertex_srv_slot() != UINT32_MAX)
                                                   ? mb.skinned_vertex_srv_slot()
                                                   : mb.vertex_srv_slot();
                            id.index_buffer_srv       = mb.index_srv_slot();
                            id.prev_vertex_buffer_srv = mb.wind_prev_pos_srv_slot();
                        }
                    }
                }
            }

            // Flush the changed GPU instance buffer entries immediately so the
            // next frame's shaders see the correct mesh/material data.
            {
                const uint32_t first = vi.tlas_instance;
                const uint32_t n     = static_cast<uint32_t>(
                    std::min(submesh_n, static_cast<size_t>(vi.tlas_slot_count)));
                if (first < m_cpu_instances.size() && n > 0)
                    m_path_tracer.upload_instance_data_range(
                        m_device_ctx, m_cpu_instances.data() + first, first, n);
            }

            // Hide any extra slots that the previous LOD used but the new LOD does not.
            // Use InstanceMask=0 so they are invisible to all ray types without
            // creating aliased GPU virtual address ranges in the TLAS.
            for (uint32_t s = static_cast<uint32_t>(submesh_n);
                 s < vi.tlas_slot_count; ++s)
            {
                const uint32_t tlas_slot = vi.tlas_instance + s;
                m_path_tracer.hide_instance(tlas_slot);
            }

            vi.current_lod = new_lod;
        }
    }
}

// ---------------------------------------------------------------------------
// update — advance animation, update rigid-node transforms
// ---------------------------------------------------------------------------
void Renderer::update(float delta_time)
{
    if (!m_initialised) return;

    // Cloth explicit-Euler integrator is unstable above ~33 ms; cap independently
    // of whatever the caller passes (which may be as large as 100 ms after a
    // window-move stall).
    static constexpr float k_max_cloth_dt = 1.0f / 15.0f;  // absolute ceiling: discard runaway frames
    m_last_delta_time          = std::min(delta_time, k_max_cloth_dt);
    m_elapsed_time_seconds    += m_last_delta_time;
    m_cloth_time_accumulator  += m_last_delta_time;

    // Advance all animation states on the CPU.
    m_anim_system.update(delta_time);

    // Update rigid-node TLAS instance transforms from their animation state.
    // For a rigid node the "bone palette" is just one matrix: the root bone transform.
    // We compose it with the base_transform to get the final world matrix.
    bool any_rigid_updated = false;
    for (auto& rn : m_scene.rigid_nodes())
    {
        if (rn.anim_state_id == UINT32_MAX) continue;
        if (rn.tlas_instance  == UINT32_MAX) continue;

        std::vector<Mat4x4> palette;
        m_anim_system.evaluate_animation(rn.anim_state_id, palette);

        // Rigid-node: use the first bone's world transform as a local delta,
        // composed on top of the base transform.
        Mat4x4 anim_local = palette.empty() ? Mat4x4::identity() : palette[0];
        Mat4x4 base       = rn.base_transform.to_matrix();
        rn.current_world  = base * anim_local;

        m_path_tracer.set_instance(rn.tlas_instance, rn.blas_index, rn.current_world, 0);
        any_rigid_updated = true;
    }

    // If any rigid node moved we need to rebuild the TLAS this frame.
    // We set a flag that render_frame_path_traced can check; for simplicity
    // we store it as a per-frame field.
    m_rigid_nodes_dirty = any_rigid_updated;

    // Cloth simulation: update is CPU-only; the GPU dispatch happens in
    // render_frame_path_traced() each frame.
    // We just mark cloth dirty here if there are any cloth instances.
    m_cloth_dirty = !m_scene.cloth_instances().empty();
}

// ---------------------------------------------------------------------------
// render_frame — dispatches to path-traced or clear-color path
// ---------------------------------------------------------------------------
void Renderer::render_frame()
{
    bool path_traced = m_path_tracer.is_initialised() && m_path_tracer.tlas_srv_slot() != UINT32_MAX;

    // Log the render path exactly once so the output isn't spammed every frame.
    if (m_frame_index == 0)
    {
        MARS_LOG("[Renderer] render_frame() path: {}",
                     path_traced ? "PATH_TRACED" : "CLEAR_COLOR_FALLBACK");
        MARS_LOG("[Renderer]   path_tracer.is_initialised() = {}",
                     m_path_tracer.is_initialised() ? "true" : "false");
        MARS_LOG("[Renderer]   path_tracer.tlas_srv_slot()  = {}",
                     m_path_tracer.tlas_srv_slot());
    }

    if (path_traced)
        render_frame_path_traced();
    else
        render_frame_clear();

    ++m_frame_index;
}

// ---------------------------------------------------------------------------
// render_frame_path_traced
// ---------------------------------------------------------------------------
void Renderer::render_frame_path_traced()
{
    uint32_t back_index = (m_display_manager.output_count() > 0)
        ? m_display_manager.output(0).current_back_buffer_index()
        : 0;

    if (m_frame_index < 3)
        MARS_LOG("[Renderer] render_frame_path_traced: frame {}", m_frame_index);

    wait_for_frame(back_index);

    // Apply per-instance LOD updates from the previous frame's GPU readback.
    // Must happen after wait_for_frame() (readback buffer is valid) and before
    // dispatch_ecosystem() (which issues a new readback copy for this frame).
    apply_vegetation_lod_updates();

    throw_if_failed(m_cmd_allocators[back_index]->Reset(), "CommandAllocator::Reset failed");
    throw_if_failed(m_cmd_list->Reset(m_cmd_allocators[back_index].Get(), nullptr),
                    "CommandList::Reset failed");

    // Bind the bindless heap so DXR shaders can access all resources.
    ID3D12DescriptorHeap* heaps[] = { m_device_ctx.bindless_heap() };
    m_cmd_list->SetDescriptorHeaps(1, heaps);

    // M10: Ecosystem per-frame GPU work (vegetation placement / LOD select).
    // Must run before any TLAS rebuild that consumes ecosystem instances.
    dispatch_ecosystem();

    // ---- GPU skinning pass --------------------------------------------------
    // For each animated scene instance: evaluate bone palette on CPU, upload it,
    // dispatch the skinning compute shader, then refit the BLAS.
    // The TLAS is rebuilt once after all refits so path tracing sees updated BVHs.
    bool any_skinned = false;
    for (const auto& inst : m_scene.instances())
    {
        if (inst.anim_state_id == UINT32_MAX) continue;
        if (inst.model_index   == UINT32_MAX) continue;

        const GpuModel& model = m_resource_mgr.model(inst.model_index);
        if (!model.has_skeleton()) continue;

        std::vector<Mat4x4> bone_palette;
        m_anim_system.evaluate_animation(inst.anim_state_id, bone_palette);

        for (uint32_t mi = 0; mi < static_cast<uint32_t>(model.mesh_buffers.size()); ++mi)
        {
            const GpuMeshBuffer& mesh = model.mesh_buffers[mi];
            if (!mesh.is_skinned()) continue;

            m_path_tracer.dispatch_skinning(m_cmd_list.Get(), mesh, bone_palette);
            m_path_tracer.refit_blas(m_device_ctx, m_cmd_list.Get(),
                                     inst.skinned_blas_base + mi, mesh);
            any_skinned = true;
        }
    }

    // ---- GPU cloth simulation pass ------------------------------------------
    // Compute how many fixed substeps to run this frame from the shared
    // accumulator.  The accumulator is drained once here so all cloth instances
    // run the same number of substeps — draining inside the per-cloth loop would
    // cause the second+ cloths to see an empty accumulator and skip simulation.
    static constexpr float k_cloth_substep_dt = 1.0f / 120.0f;
    const uint32_t substep_count = static_cast<uint32_t>(m_cloth_time_accumulator / k_cloth_substep_dt);
    m_cloth_time_accumulator -= static_cast<float>(substep_count) * k_cloth_substep_dt;

    if (m_frame_index == 0)
        MARS_LOG("[Renderer] Cloth instances at render time: {}", m_scene.cloth_instances().size());

    for (auto& ci : m_scene.cloth_instances())
    {
        if (m_frame_index == 0)
        {
            MARS_LOG("[Renderer] Cloth '{}': blas={} tlas={} vtx_count={} idx_count={}"
                         " out_vtx_srv={} idx_srv={} out_vtx_res={} idx_res={}"
                         " pos_prev_srv={} pos_curr_srv={} pos_pred_a_uav={} out_vtx_uav={}",
                         ci.name, ci.cloth_blas_index, ci.tlas_instance,
                         ci.vertex_count, ci.index_count,
                         ci.gpu.output_vertex_srv(), ci.mesh_buffer.index_srv_slot(),
                         static_cast<void*>(ci.gpu.output_vertex_resource()),
                         static_cast<void*>(ci.mesh_buffer.index_buffer_resource()),
                         ci.gpu.pos_prev_srv(), ci.gpu.pos_curr_srv(),
                         ci.gpu.pos_pred_a_uav(), ci.gpu.output_vertex_uav());
        }

        if (ci.cloth_blas_index == UINT32_MAX)
        {
            if (m_frame_index == 0) MARS_LOG("[Renderer]   SKIP: cloth_blas_index == UINT32_MAX");
            continue;
        }
        if (!ci.gpu.is_valid())
        {
            if (m_frame_index == 0) MARS_LOG("[Renderer]   SKIP: gpu not valid");
            continue;
        }

        if (substep_count == 0)
            continue;  // no substep this frame — skip refit, nothing changed

        for (uint32_t step = 0; step < substep_count; ++step)
        {
            m_path_tracer.dispatch_cloth_sim(m_cmd_list.Get(), ci, k_cloth_substep_dt, m_elapsed_time_seconds);
        }

        m_path_tracer.refit_blas(m_device_ctx, m_cmd_list.Get(), ci.cloth_blas_index,
                                 ci.gpu.output_vertex_resource(),
                                 ci.vertex_count, ci.index_count,
                                 ci.mesh_buffer.index_buffer_resource());
    }

    // ---- GPU vegetation wind deformation + BLAS refit ----------------------
    // For each active species / LOD / submesh: deform the rest-pose vertex
    // buffer into the output (wind) buffer via the wind compute shader, issue
    // a UAV barrier, then refit the refittable BLAS. All placed instances of
    // the same species share the same BLAS, so one dispatch per submesh is
    // enough regardless of instance count.
    bool any_vegetation_wind    = false;
    bool any_ecosystem_active   = false;   // true when any placed ecosystem is visible
    {
        auto& ecosystems = m_scene.ecosystems();
        for (size_t layer_idx = 0; layer_idx < ecosystems.size(); ++layer_idx)
        {
            EcosystemDesc& eco = ecosystems[layer_idx];
            if (!eco.enabled) continue;
            if (layer_idx >= m_ecosystem_placed.size() || !m_ecosystem_placed[layer_idx]) continue;
            if (eco.instance_count > 0)
                any_ecosystem_active = true;

            constexpr VegetationLOD k_wind_lods[] = {
                VegetationLOD::Near, VegetationLOD::Mid, VegetationLOD::FarCluster };

            for (size_t sp_idx = 0; sp_idx < eco.species.size(); ++sp_idx)
            {
                SpeciesDesc& sp = eco.species[sp_idx];

                const uint32_t hash_val = static_cast<uint32_t>(sp_idx) * 2654435761u;
                const float    sp_phase = static_cast<float>(hash_val >> 8) / static_cast<float>(1u << 24);
                for (VegetationLOD lod : k_wind_lods)
                {
                    const auto li = static_cast<size_t>(lod);
                    if (sp.model_indices[li] == UINT32_MAX) continue;
                    if (sp.blas_indices[li].empty())        continue;

                    const GpuModel& gm = m_resource_mgr.model(sp.model_indices[li]);

                    for (size_t si = 0; si < sp.blas_indices[li].size(); ++si)
                    {
                        const uint32_t blas_idx = sp.blas_indices[li][si];
                        if (blas_idx == UINT32_MAX)              continue;
                        if (si >= gm.mesh_buffers.size())        break;

                        const GpuMeshBuffer& mb = gm.mesh_buffers[si];
                        if (!mb.skinned_vertex_buffer())         continue;
                        if (mb.skinned_vertex_uav_slot() == UINT32_MAX) continue;

                        const float mesh_min_y  = gm.bounds.min_pt.y;
                        const float mesh_height = gm.bounds.max_pt.y - gm.bounds.min_pt.y;

                        const bool is_leaf = (si < gm.mesh_is_leaf.size()) && gm.mesh_is_leaf[si];

                        m_path_tracer.dispatch_vegetation_wind(
                            m_cmd_list.Get(),
                            mb.vertex_count(),
                            mb.vertex_srv_slot(),
                            mb.skinned_vertex_uav_slot(),
                            mb.wind_prev_pos_uav_slot(),
                            mesh_min_y,
                            mesh_height,
                            m_elapsed_time_seconds,
                            sp_phase * 6.28318f,
                            sp.primary_bend_strength,
                            sp.primary_bend_speed,
                            sp.wind_leaf_flutter_strength,
                            sp.wind_leaf_flutter_speed,
                            is_leaf);

                        D3D12_RESOURCE_BARRIER uav_barrier{};
                        uav_barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        uav_barrier.UAV.pResource = mb.skinned_vertex_buffer();
                        m_cmd_list->ResourceBarrier(1, &uav_barrier);

                        m_path_tracer.refit_blas(m_device_ctx, m_cmd_list.Get(),
                                                 blas_idx, mb);
                        any_vegetation_wind = true;
                    }
                }
            }
        }
    }

    // Rebuild TLAS once if any BLAS was refitted or ecosystem instances changed
    // visibility / LOD this frame (culling + LOD selection both modify the GPU
    // instance buffer; the TLAS must see the updated acceleration structures).
    if (any_skinned || m_rigid_nodes_dirty || m_cloth_dirty ||
        any_vegetation_wind || any_ecosystem_active)
        rebuild_tlas();

    for (uint32_t oi = 0; oi < m_display_manager.output_count(); ++oi)
    {
        // Update per-frame constants — pull sun from the first directional light in the scene
        const CameraState& cam = (oi < m_cameras.size()) ? m_cameras[oi] : m_cameras[0];

        Vec3  sun_dir       = { 0.3f,  0.8f, 0.5f };
        Vec3  sun_color     = { 1.0f, 0.95f, 0.85f };
        float sun_intensity = 5.0f;
        for (const auto& light : m_scene.lights())
        {
            if (light.type == LightType::Directional)
            {
                sun_dir       = light.direction;
                sun_color     = light.color;
                sun_intensity = light.intensity;
                break;
            }
        }

        m_path_tracer.update_frame_constants(m_device_ctx, oi, m_frame_index,
                                             cam.position, cam.view_inv, cam.proj_inv,
                                             sun_dir, sun_color, sun_intensity,
                                             cam.prev_view_proj);

        // Trace rays → UAV
        m_path_tracer.trace(m_cmd_list.Get(), oi, m_frame_index);

        // UAV barrier between trace and DLSS / copy
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = nullptr; // all UAVs
        m_cmd_list->ResourceBarrier(1, &uav);

        // DLSS resource tagging + evaluation (no-op if DLSS is not supported/init'd).
        bool rr_evaluated = false;
        if (m_denoiser.is_initialised())
        {
            DisplayOutput& disp = m_display_manager.output(oi);
            uint32_t rw = disp.width(), rh = disp.height();
            uint32_t dw = disp.width(), dh = disp.height();
            m_denoiser.get_render_resolution(oi, rw, rh);

            // Streamline's DLSS-D internally issues legacy ResourceBarriers on the
            // noisy color, motion vector, and depth resources, assuming they are in
            // D3D12_RESOURCE_STATE_COMMON. The path tracer writes them as UAVs via
            // DispatchRays, leaving them in D3D12_RESOURCE_STATE_UNORDERED_ACCESS
            // (legacy-tracked). Use a plain legacy transition barrier (UAV → COMMON)
            // rather than an Enhanced Barrier: Enhanced Barriers are only valid on
            // legacy-tracked resources when the layout is already COMMON, so issuing
            // an Enhanced Barrier with LayoutBefore=UNORDERED_ACCESS on a resource
            // that has never been touched by the Enhanced Barrier API causes
            // BARRIER_INTEROP_INVALID_LAYOUT (#1350) at ExecuteCommandLists.
            {
                ID3D12Resource* pt_resources[] = {
                    m_path_tracer.output_resource(oi),
                    m_path_tracer.motion_vector_resource(oi),
                    m_path_tracer.depth_resource(oi),
                    m_path_tracer.albedo_resource(oi),
                    m_path_tracer.specular_albedo_resource(oi),
                    m_path_tracer.normals_aov_resource(oi),
                    m_path_tracer.roughness_aov_resource(oi),
                };

                D3D12_RESOURCE_BARRIER to_common[7]{};
                uint32_t barrier_count = 0;
                for (ID3D12Resource* res : pt_resources)
                {
                    if (!res) continue;
                    D3D12_RESOURCE_BARRIER& b    = to_common[barrier_count++];
                    b.Type                       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Flags                      = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    b.Transition.pResource       = res;
                    b.Transition.StateBefore     = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    b.Transition.StateAfter      = D3D12_RESOURCE_STATE_COMMON;
                    b.Transition.Subresource     = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                }
                if (barrier_count > 0)
                    m_cmd_list->ResourceBarrier(barrier_count, to_common);
            }

            m_denoiser.tag_resources(
                m_cmd_list.Get(), oi, m_frame_index,
                m_path_tracer.output_resource(oi),           // noisy color IN
                m_path_tracer.motion_vector_resource(oi),
                m_path_tracer.depth_resource(oi),
                m_path_tracer.albedo_resource(oi),           // albedo AOV (dedicated, not aliased)
                m_path_tracer.specular_albedo_resource(oi),  // specular albedo AOV
                m_path_tracer.normals_aov_resource(oi),      // normals AOV (black placeholder until M7/M8)
                m_path_tracer.roughness_aov_resource(oi),    // roughness AOV (black placeholder until M7/M8)
                m_path_tracer.denoised_output_resource(oi),  // denoised color OUT
                rw, rh, dw, dh);

            // DLSS-RR denoises and upscales in a single pass.
            // DLSS-SR is mutually exclusive with DLSS-RR: both features share the
            // same internal Streamline motion-vector buffer (sl.dlss.mvec), and
            // chaining them in the same command list causes a
            // RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH at ExecuteCommandLists.
            // Only run SR when RR is not active.
            Mat4x4 view = cam.view_inv.inverse();
            Mat4x4 proj = cam.proj_inv.inverse();
            rr_evaluated = m_denoiser.evaluate_dlss_rr(
                                        m_cmd_list.Get(), oi, m_frame_index,
                                        view, cam.view_inv, proj,
                                        cam.prev_view_inv, cam.prev_proj_inv,
                                        0.0f, 0.0f, rw, rh, dw, dh);
            if (!m_denoiser.dlss_rr_supported())
            {
                m_denoiser.evaluate_dlss_sr(m_cmd_list.Get(), oi, m_frame_index,
                                            0.0f, 0.0f, rw, rh, dw, dh);
            }

            // Streamline always issues a COMMON→UAV barrier on DenoisedOutputUAV
            // before writing, regardless of whether slEvaluateFeature ultimately
            // succeeds. Track this so we can clean up on the following frame if
            // the call failed.
            // IMPORTANT: only set this flag when slEvaluateFeature actually ran —
            // i.e. when SL passed tag validation and issued GPU barriers. If SL
            // rejected the call at tag-validation time (e.g. a required buffer
            // tag was missing), it never touched DenoisedOutputUAV and the flag
            // must stay false to avoid a spurious UAV→COMMON cleanup barrier on
            // the next frame that would trigger RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH.
            if (rr_evaluated)
            {
                if (oi < m_denoised_in_uav_state.size())
                    m_denoised_in_uav_state[oi] = true;
                if (oi < m_output_in_psr_state.size())
                    m_output_in_psr_state[oi] = true;
            }

            // After slEvaluateFeature (DLSS-RR), DenoisedOutputUAV is left in
            // D3D12_RESOURCE_STATE_UNORDERED_ACCESS (Streamline transitions it from
            // COMMON to UAV before writing via legacy ResourceBarrier). Transition it
            // back to COMMON so copy_to_back_buffer can bind it as an SRV.
            // IMPORTANT: only emit this barrier when slEvaluateFeature actually ran.
            // If it was skipped, DenoisedOutputUAV was never touched and remains in
            // COMMON — issuing a UAV→COMMON barrier on a COMMON resource causes
            // RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH #527.
            if (rr_evaluated)
            {
                ID3D12Resource* denoised_res = m_path_tracer.denoised_output_resource(oi);
                if (denoised_res)
                {
                    // Streamline writes to DenoisedOutputUAV using legacy ResourceBarriers
                    // (COMMON → UAV → write), so the Enhanced Barrier layout remains COMMON
                    // throughout. Issue a legacy UAV → COMMON transition here so that
                    // copy_to_back_buffer can read it as an SRV on the same command list.
                    // A legacy transition to/from COMMON is always valid when the Enhanced
                    // layout is COMMON, which it is because Streamline never used an Enhanced
                    // Barrier on this resource.
                    D3D12_RESOURCE_BARRIER dn_to_common{};
                    dn_to_common.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    dn_to_common.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    dn_to_common.Transition.pResource   = denoised_res;
                    dn_to_common.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    dn_to_common.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
                    dn_to_common.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    m_cmd_list->ResourceBarrier(1, &dn_to_common);

                    if (oi < m_denoised_in_uav_state.size())
                        m_denoised_in_uav_state[oi] = false;
                }
            }
            else
            {
                // slEvaluateFeature failed this frame, but Streamline may have already
                // issued a COMMON→UAV legacy barrier on DenoisedOutputUAV before failing.
                // If the flag is set from a previous successful or partial frame, issue
                // a legacy UAV→COMMON cleanup barrier so the next frame sees COMMON.
                ID3D12Resource* denoised_res = m_path_tracer.denoised_output_resource(oi);
                if (denoised_res &&
                    oi < m_denoised_in_uav_state.size() &&
                    m_denoised_in_uav_state[oi])
                {
                    D3D12_RESOURCE_BARRIER cleanup{};
                    cleanup.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    cleanup.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    cleanup.Transition.pResource   = denoised_res;
                    cleanup.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    cleanup.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
                    cleanup.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    m_cmd_list->ResourceBarrier(1, &cleanup);
                    m_denoised_in_uav_state[oi] = false;
                }
            }

            // Transition the three path-tracer input resources back to UAV so the
            // next frame's DispatchRays can write to them. These are legacy-tracked
            // resources and must stay in UAV state between frames.
            //
            // After slEvaluateFeature (DLSS-RR), Streamline reads these resources as
            // shader inputs (SRV) and leaves them in D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE.
            // StateBefore must therefore be PIXEL_SHADER_RESOURCE, not COMMON.
            // Using COMMON here causes RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH #527
            // because the D3D12 runtime's tracked state is 0x80, not 0x0.
            {
                ID3D12Resource* pt_resources[] = {
                    m_path_tracer.output_resource(oi),
                    m_path_tracer.motion_vector_resource(oi),
                    m_path_tracer.depth_resource(oi),
                    m_path_tracer.albedo_resource(oi),
                    m_path_tracer.specular_albedo_resource(oi),
                    m_path_tracer.normals_aov_resource(oi),
                    m_path_tracer.roughness_aov_resource(oi),
                };

                D3D12_RESOURCE_BARRIER to_uav[7]{};
                uint32_t barrier_count = 0;
                for (ID3D12Resource* res : pt_resources)
                {
                    if (!res) continue;
                    D3D12_RESOURCE_BARRIER& b    = to_uav[barrier_count++];
                    b.Type                       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Flags                      = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    b.Transition.pResource       = res;
                    b.Transition.StateBefore     = rr_evaluated
                            ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE  // DLSS-RR consumed resources as SRVs
                            : D3D12_RESOURCE_STATE_COMMON;                 // DLSS-RR skipped — resources still in COMMON
                    // After the UAV restore barrier below, output_resource is back in UAV.
                    if (res == m_path_tracer.output_resource(oi) &&
                        oi < m_output_in_psr_state.size())
                        m_output_in_psr_state[oi] = false;
                    b.Transition.StateAfter      = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    b.Transition.Subresource     = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                }
                if (barrier_count > 0)
                    m_cmd_list->ResourceBarrier(barrier_count, to_uav);
            }
        }

        // Re-bind the bindless heap — Streamline (DLSS) may have replaced it
        // with its own internal heap during denoiser evaluation.
        {
            ID3D12DescriptorHeap* bindless_heaps[] = { m_device_ctx.bindless_heap() };
            m_cmd_list->SetDescriptorHeaps(1, bindless_heaps);
        }

        // Transition back buffer: PRESENT → RENDER_TARGET
        DisplayOutput& display_out = m_display_manager.output(oi);
        display_out.transition(m_cmd_list.Get(),
                       D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);

        // Blit to back buffer with tone-mapping.
        // When the denoiser is active, read from the denoised output texture;
        // otherwise read directly from the raw path-tracer UAV.
        // IMPORTANT: only read DenoisedOutputUAV when slEvaluateFeature actually ran
        // this frame. If it was skipped — e.g. because Streamline's internal
        // sl.dlss_d.mvec barrier mismatch caused the call to fail — the post-evaluate
        // Enhanced Barrier (UAV→COMMON layout) was also skipped, leaving
        // DenoisedOutputUAV in D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS. Binding it as an
        // SRV in that layout triggers GPU_BASED_VALIDATION_INCOMPATIBLE_TEXTURE_LAYOUT
        // #1358 and crashes ExecuteCommandLists with 0x87A.
        bool use_denoised = rr_evaluated &&
                            m_path_tracer.denoised_output_resource(oi) != nullptr;
        m_path_tracer.copy_to_back_buffer(m_cmd_list.Get(), oi,
                                           display_out.current_back_buffer(),
                                           display_out.current_rtv(),
                                           static_cast<uint32_t>(display_out.hdr_mode()),
                                           display_out.back_buffer_format(),
                                           use_denoised);

        // Transition back buffer: RENDER_TARGET → PRESENT
        display_out.transition(m_cmd_list.Get(),
                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PRESENT);
    }

    throw_if_failed(m_cmd_list->Close(), "CommandList::Close failed");

    ID3D12CommandList* lists[] = { m_cmd_list.Get() };
    m_device_ctx.direct_queue()->ExecuteCommandLists(1, lists);

    // DLSS-G Multi Frame Generation runs after command list execution, before Present.
    // The engine command list is already closed and submitted at this point.
    // DLSS-G works by hooking Present via Streamline's interposer and does not
    // require the engine's command list — pass nullptr for the command buffer.
    if (m_denoiser.is_initialised() && m_denoiser.dlss_g_supported())
    {
        // Each output gets its own MFG invocation.
        for (uint32_t oi = 0; oi < m_display_manager.output_count(); ++oi)
            m_denoiser.evaluate_dlss_g(nullptr, oi, m_frame_index);
    }

    for (uint32_t oi = 0; oi < m_display_manager.output_count(); ++oi)
        m_display_manager.output(oi).present(true);

    ++m_frame_fence_next;
    m_device_ctx.direct_queue()->Signal(m_frame_fence.Get(), m_frame_fence_next);
    m_frame_fence_values[back_index] = m_frame_fence_next;
}

// ---------------------------------------------------------------------------
// render_frame_clear  (M2 fallback: solid-color clear + present)
// ---------------------------------------------------------------------------
void Renderer::render_frame_clear()
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

// ---------------------------------------------------------------------------
// save_screenshot
// ---------------------------------------------------------------------------
// Saves the denoised (and raw noisy) output buffers as PNG files.
// File names: screenshot_<timestamp>_denoised.png, screenshot_<timestamp>_raw.png
// Both are tone-mapped via DirectXTex convert (RGBA16F → RGBA8_UNORM).
// ---------------------------------------------------------------------------
void Renderer::save_screenshot(uint32_t output_index)
{
    MARS_LOG("[Screenshot] save_screenshot() called for output_index={}", output_index);

    // Flush GPU so the frame is complete before we read it back.
    MARS_LOG("[Screenshot] Flushing GPU (direct queue)...");
    m_device_ctx.flush_direct_queue();
    MARS_LOG("[Screenshot] Direct queue flush complete.");
    MARS_LOG("[Screenshot] Flushing GPU (compute queue)...");
    m_device_ctx.flush_compute_queue();
    MARS_LOG("[Screenshot] Compute queue flush complete.");
    MARS_LOG("[Screenshot] Flushing GPU (copy queue)...");
    m_device_ctx.flush_copy_queue();
    MARS_LOG("[Screenshot] Copy queue flush complete.");

    // Helper: read back one RGBA16F texture → save as PNG.
    auto readback_and_save = [&](ID3D12Resource* src,
                                   D3D12_RESOURCE_STATES src_state,
                                   const std::wstring& path)
    {
        const std::string path_str = std::filesystem::path(path).string();
        MARS_LOG("[Screenshot] readback_and_save: '{}', src={}, src_state={:#x}",
                 path_str, static_cast<void*>(src), static_cast<uint32_t>(src_state));

        if (!src)
        {
            MARS_LOG("[Screenshot] Source resource is null, skipping '{}'.", path_str);
            return;
        }

        D3D12_RESOURCE_DESC desc = src->GetDesc();
        const uint32_t w = static_cast<uint32_t>(desc.Width);
        const uint32_t h = desc.Height;
        MARS_LOG("[Screenshot]   texture: {}x{}, format={:#x}, flags={:#x}",
                 w, h, static_cast<uint32_t>(desc.Format), static_cast<uint32_t>(desc.Flags));

        // Calculate the row pitch aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT.
        const uint32_t bytes_per_pixel = 8u; // RGBA16F = 4 * 2 bytes
        const uint32_t row_pitch = (w * bytes_per_pixel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u)
                                   & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
        const uint64_t total_bytes = static_cast<uint64_t>(row_pitch) * h;
        MARS_LOG("[Screenshot]   row_pitch={}, total_bytes={}", row_pitch, total_bytes);

        // Create a READBACK buffer.
        MARS_LOG("[Screenshot]   Creating readback buffer...");
        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buf_desc{};
        buf_desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width            = total_bytes;
        buf_desc.Height           = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels        = 1;
        buf_desc.Format           = DXGI_FORMAT_UNKNOWN;
        buf_desc.SampleDesc       = { 1, 0 };
        buf_desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buf_desc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3D12Resource> readback_buf;
        HRESULT hr = m_device_ctx.device()->CreateCommittedResource(
            &heap_props, D3D12_HEAP_FLAG_NONE, &buf_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback_buf));
        if (FAILED(hr))
        {
            MARS_LOG("[Screenshot] Failed to create readback buffer (hr={:#x}).", static_cast<uint32_t>(hr));
            return;
        }
        MARS_LOG("[Screenshot]   Readback buffer created OK.");

        // Record the copy.
        MARS_LOG("[Screenshot]   Resetting cmd allocator [0]...");
        throw_if_failed(m_cmd_allocators[0]->Reset(), "Screenshot: CmdAlloc::Reset");
        MARS_LOG("[Screenshot]   Resetting cmd list...");
        throw_if_failed(m_cmd_list->Reset(m_cmd_allocators[0].Get(), nullptr), "Screenshot: CmdList::Reset");
        MARS_LOG("[Screenshot]   Command list reset OK.");

        // Transition source to COPY_SOURCE.
        MARS_LOG("[Screenshot]   Recording barrier: state {:#x} -> COPY_SOURCE ({:#x})...",
                 static_cast<uint32_t>(src_state),
                 static_cast<uint32_t>(D3D12_RESOURCE_STATE_COPY_SOURCE));
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = src;
        barrier.Transition.StateBefore = src_state;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_cmd_list->ResourceBarrier(1, &barrier);
        MARS_LOG("[Screenshot]   Barrier recorded.");

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource        = src;
        src_loc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_loc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource                          = readback_buf.Get();
        dst_loc.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint.Offset             = 0;
        dst_loc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dst_loc.PlacedFootprint.Footprint.Width    = w;
        dst_loc.PlacedFootprint.Footprint.Height   = h;
        dst_loc.PlacedFootprint.Footprint.Depth    = 1;
        dst_loc.PlacedFootprint.Footprint.RowPitch = row_pitch;

        MARS_LOG("[Screenshot]   Recording CopyTextureRegion...");
        m_cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
        MARS_LOG("[Screenshot]   CopyTextureRegion recorded.");

        // Transition back.
        MARS_LOG("[Screenshot]   Recording barrier back: COPY_SOURCE -> {:#x}...",
                 static_cast<uint32_t>(src_state));
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        m_cmd_list->ResourceBarrier(1, &barrier);
        MARS_LOG("[Screenshot]   Return barrier recorded.");

        MARS_LOG("[Screenshot]   Closing command list...");
        throw_if_failed(m_cmd_list->Close(), "Screenshot: CmdList::Close");
        MARS_LOG("[Screenshot]   Executing command list...");
        ID3D12CommandList* lists[] = { m_cmd_list.Get() };
        m_device_ctx.direct_queue()->ExecuteCommandLists(1, lists);
        MARS_LOG("[Screenshot]   Flushing GPU after copy...");
        m_device_ctx.flush_gpu();
        MARS_LOG("[Screenshot]   GPU flush after copy complete.");

        // Map the readback buffer.
        MARS_LOG("[Screenshot]   Mapping readback buffer...");
        void* mapped = nullptr;
        hr = readback_buf->Map(0, nullptr, &mapped);
        if (FAILED(hr))
        {
            MARS_LOG("[Screenshot] Failed to map readback buffer (hr={:#x}).", static_cast<uint32_t>(hr));
            return;
        }
        MARS_LOG("[Screenshot]   Readback buffer mapped at {:p}.", mapped);

        // Build a DirectXTex image from the mapped RGBA16F data.
        DirectX::Image img{};
        img.width      = w;
        img.height     = h;
        img.format     = DXGI_FORMAT_R16G16B16A16_FLOAT;
        img.rowPitch   = row_pitch;
        img.slicePitch = static_cast<size_t>(row_pitch) * h;
        img.pixels     = static_cast<uint8_t*>(mapped);

        // Convert RGBA16F → RGBA8_UNORM with simple tone-mapping (clamp).
        MARS_LOG("[Screenshot]   Converting RGBA16F -> RGBA8...");
        DirectX::ScratchImage converted;
        hr = DirectX::Convert(img, DXGI_FORMAT_R8G8B8A8_UNORM,
                              DirectX::TEX_FILTER_DEFAULT | DirectX::TEX_FILTER_FORCE_NON_WIC,
                              DirectX::TEX_THRESHOLD_DEFAULT, converted);

        readback_buf->Unmap(0, nullptr);
        MARS_LOG("[Screenshot]   Readback buffer unmapped.");

        if (FAILED(hr))
        {
            MARS_LOG("[Screenshot] DirectXTex Convert failed (hr={:#x}).", static_cast<uint32_t>(hr));
            return;
        }
        MARS_LOG("[Screenshot]   Convert OK. Saving PNG to '{}'...", path_str);

        // Save as PNG using WIC.
        hr = DirectX::SaveToWICFile(*converted.GetImages(), DirectX::WIC_FLAGS_NONE,
                                    GUID_ContainerFormatPng, path.c_str());
        if (SUCCEEDED(hr))
            MARS_LOG("[Screenshot] Saved: {}", path_str);
        else
            MARS_LOG("[Screenshot] SaveToWICFile failed (hr={:#x}) for '{}'.", static_cast<uint32_t>(hr), path_str);
    };

    // Build a timestamp string for unique filenames.
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const std::wstring ts = std::format(L"{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}",
                                        st.wYear, st.wMonth, st.wDay,
                                        st.wHour, st.wMinute, st.wSecond);
    MARS_LOG("[Screenshot] Timestamp: {}", std::filesystem::path(ts).string());

    // Ensure the dumps output directory exists.
    const std::filesystem::path dump_dir = "dumps";
    std::filesystem::create_directories(dump_dir);

    // Log what state flags say about each output resource.
    MARS_LOG("[Screenshot] m_output_in_psr_state size={}, output_index={}",
             m_output_in_psr_state.size(), output_index);
    if (output_index < m_output_in_psr_state.size())
        MARS_LOG("[Screenshot] m_output_in_psr_state[{}]={}", output_index, m_output_in_psr_state[output_index]);

    // denoised_output_resource ends the frame in COMMON: render_frame_path_traced
    // explicitly transitions it UAV→COMMON after slEvaluateFeature (DLSS-RR).
    // output_resource (raw noisy) ends the frame in UNORDERED_ACCESS: it is
    // restored to UAV at the end of the per-output barrier block.
    MARS_LOG("[Screenshot] --- Capturing denoised output ---");
    readback_and_save(m_path_tracer.denoised_output_resource(output_index),
                      D3D12_RESOURCE_STATE_COMMON,
                      (dump_dir / std::format(L"screenshot_{}_denoised.png", ts)).wstring());
    // output_resource ends each frame in UAV, UNLESS DLSS-RR ran and the
    // per-output PSR tracking flag was not yet cleared (e.g. screenshot taken
    // immediately after the frame fence wait, before the barrier loop ran).
    // In practice flush_gpu() ensures the frame is complete, but the CPU-side
    // flag reflects the barrier state that was recorded into the command list,
    // so honour it to avoid RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH.
    const D3D12_RESOURCE_STATES raw_state =
        (output_index < m_output_in_psr_state.size() && m_output_in_psr_state[output_index])
        ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    MARS_LOG("[Screenshot] --- Capturing raw output (assumed state={:#x}) ---",
             static_cast<uint32_t>(raw_state));
    readback_and_save(m_path_tracer.output_resource(output_index),
                      raw_state,
                      (dump_dir / std::format(L"screenshot_{}_raw.png", ts)).wstring());

    // ---- Scene / camera JSON dump -------------------------------------------
    {
        const std::string json_path = (dump_dir / std::format("screenshot_{}.json",
                                                   std::filesystem::path(ts).string())).string();

        std::ofstream f(json_path);
        if (f)
        {
            // Tiny helpers to write JSON without pulling in a full library.
            auto jvec3 = [](const Vec3& v) {
                return std::format("[{:.4f}, {:.4f}, {:.4f}]", v.x, v.y, v.z);
            };
            auto jfloat = [](float v) { return std::format("{:.4f}", v); };

            f << "{\n";

            // ---- frame info -----------------------------------------------
            f << "  \"frame_timestamp\": \"" << std::filesystem::path(ts).string() << "\",\n";

            // ---- active camera (runtime state) ----------------------------
            f << "  \"active_camera\": {\n";
            if (output_index < m_cameras.size())
            {
                const CameraState& cs = m_cameras[output_index];

                // Extract forward, right, up from the inverse-view matrix
                // (columns of the rotation part in a standard view-inv).
                // view_inv col 0 = right, col 1 = up, col 2 = forward (into scene).
                Vec3 right   = { cs.view_inv.m[0][0], cs.view_inv.m[1][0], cs.view_inv.m[2][0] };
                Vec3 up      = { cs.view_inv.m[0][1], cs.view_inv.m[1][1], cs.view_inv.m[2][1] };
                Vec3 forward = { cs.view_inv.m[0][2], cs.view_inv.m[1][2], cs.view_inv.m[2][2] };

                f << "    \"position\": " << jvec3(cs.position) << ",\n";
                f << "    \"forward\":  " << jvec3(forward)     << ",\n";
                f << "    \"up\":       " << jvec3(up)          << ",\n";
                f << "    \"right\":    " << jvec3(right)       << "\n";
            }
            f << "  },\n";

            // ---- scene name / skybox --------------------------------------
            const Scene&     sc  = m_scene;
            const SkyboxDesc& sky = sc.skybox();
            const char* sky_type_str = "debug";
            switch (sky.type)
            {
                case SkyboxDesc::Type::Physical: sky_type_str = "physical"; break;
                case SkyboxDesc::Type::HDRI:     sky_type_str = "hdri";     break;
                case SkyboxDesc::Type::Black:    sky_type_str = "black";    break;
                default: break;
            }
            f << "  \"scene_name\": \"" << sc.name() << "\",\n";
            f << "  \"skybox\": {\n";
            f << "    \"type\": \"" << sky_type_str << "\",\n";
            f << "    \"sun_direction\": " << jvec3(sky.sun_direction) << ",\n";
            f << "    \"sun_color\": "     << jvec3(sky.sun_color)     << ",\n";
            f << "    \"sun_intensity\": " << jfloat(sky.sun_intensity) << "\n";
            f << "  },\n";

            // ---- scene lights ---------------------------------------------
            f << "  \"lights\": [\n";
            for (size_t i = 0; i < sc.lights().size(); ++i)
            {
                const LightDesc& l = sc.lights()[i];
                const char* lt = l.type == LightType::Point  ? "point"
                               : l.type == LightType::Spot   ? "spot"
                               : "directional";
                f << "    {\n";
                f << "      \"name\": \""      << l.name      << "\",\n";
                f << "      \"type\": \""      << lt          << "\",\n";
                f << "      \"direction\": "   << jvec3(l.direction)  << ",\n";
                f << "      \"position\": "    << jvec3(l.position)   << ",\n";
                f << "      \"color\": "       << jvec3(l.color)      << ",\n";
                f << "      \"intensity\": "   << jfloat(l.intensity) << "\n";
                f << "    }" << (i + 1 < sc.lights().size() ? "," : "") << "\n";
            }
            f << "  ],\n";

            // ---- model instances -----------------------------------------
            f << "  \"models\": [\n";
            for (size_t i = 0; i < sc.instances().size(); ++i)
            {
                const SceneModelInstance& inst = sc.instances()[i];
                const Transform& t = inst.transform;
                f << "    {\n";
                f << "      \"name\": \""     << inst.name     << "\",\n";
                f << "      \"position\": "   << jvec3(t.position) << ",\n";
                f << "      \"scale\": "      << jfloat(t.scale)   << ",\n";
                f << "      \"rotation\": [" << std::format("{:.4f}, {:.4f}, {:.4f}, {:.4f}",
                                                            t.rotation.x, t.rotation.y,
                                                            t.rotation.z, t.rotation.w) << "]\n";
                f << "    }" << (i + 1 < sc.instances().size() ? "," : "") << "\n";
            }
            f << "  ],\n";

            // ---- ecosystems ----------------------------------------------
            f << "  \"ecosystems\": [\n";
            for (size_t ei = 0; ei < sc.ecosystems().size(); ++ei)
            {
                const EcosystemDesc& eco = sc.ecosystems()[ei];
                f << "    {\n";
                f << "      \"enabled\": "         << (eco.enabled ? "true" : "false") << ",\n";
                f << "      \"instance_count\": "  << eco.instance_count << ",\n";
                f << "      \"world_min\": "        << jvec3(eco.world_min) << ",\n";
                f << "      \"world_max\": "        << jvec3(eco.world_max) << ",\n";
                f << "      \"placement_y\": "      << jfloat(eco.placement_y) << ",\n";
                f << "      \"lod_dither_band_meters\": " << jfloat(eco.lod_dither_band_meters) << ",\n";
                f << "      \"species\": [\n";
                for (size_t si = 0; si < eco.species.size(); ++si)
                {
                    const SpeciesDesc& sp = eco.species[si];
                    f << "        {\n";
                    f << "          \"name\": \""           << sp.name          << "\",\n";
                    f << "          \"lod_near_max\": "     << jfloat(sp.lod_near_max)     << ",\n";
                    f << "          \"lod_mid_max\": "      << jfloat(sp.lod_mid_max)      << ",\n";
                    f << "          \"lod_far_max\": "      << jfloat(sp.lod_far_max)      << ",\n";
                    f << "          \"max_draw_distance\": "<< jfloat(sp.max_draw_distance) << "\n";
                    f << "        }" << (si + 1 < eco.species.size() ? "," : "") << "\n";
                }
                f << "      ],\n";
                // Dump first few placed instances for spatial context
                const auto& insts = eco.instances;
                f << "      \"placed_instance_count\": " << insts.size() << ",\n";
                f << "      \"placed_instances_sample\": [\n";
                const size_t sample_max = std::min(insts.size(), size_t(20));
                for (size_t ii = 0; ii < sample_max; ++ii)
                {
                    const VegetationInstance& vi = insts[ii];
                    const char* lod_str =
                        vi.current_lod == VegetationLOD::Near        ? "Near"
                      : vi.current_lod == VegetationLOD::Mid         ? "Mid"
                      : vi.current_lod == VegetationLOD::FarCluster  ? "FarCluster"
                      : vi.current_lod == VegetationLOD::Impostor    ? "Impostor"
                      : "Culled";
                    // Derive Y-axis rotation in degrees from the quaternion (vegetation
                    // is only ever rotated around world-Y by the placement shader).
                    const Quaternion& q = vi.transform.rotation;
                    const float yaw_rad = 2.0f * std::atan2(q.y, q.w);
                    const float yaw_deg = yaw_rad * (180.0f / 3.14159265358979323846f);
                    f << "        {\n";
                    f << "          \"pos\": "      << jvec3(vi.transform.position) << ",\n";
                    f << "          \"rotation_quat\": [" << std::format("{:.4f}, {:.4f}, {:.4f}, {:.4f}", q.x, q.y, q.z, q.w) << "],\n";
                    f << "          \"yaw_deg\": "  << std::format("{:.2f}", yaw_deg) << ",\n";
                    f << "          \"scale\": "    << jfloat(vi.transform.scale) << ",\n";
                    f << "          \"species\": "  << vi.species_index << ",\n";
                    f << "          \"lod\": \""    << lod_str << "\"\n";
                    f << "        }" << (ii + 1 < sample_max ? "," : "") << "\n";
                }
                f << "      ]\n";
                f << "    }" << (ei + 1 < sc.ecosystems().size() ? "," : "") << "\n";
            }
            f << "  ]\n";

            f << "}\n";
            f.flush();
            MARS_LOG("[Screenshot] Scene JSON saved: {}", json_path);
        }
        else
        {
            MARS_LOG("[Screenshot] WARNING: could not open '{}' for writing.", json_path);
        }
    }

    MARS_LOG("[Screenshot] Done.");
}

} // namespace mars
