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
#include <windows.h>

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
    m_ecosystem_gpu.destroy();
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

    cam.position = position;
    cam.view_inv = view_inv;
    cam.proj_inv = proj_inv;
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

    m_wind_desc    = m_scene.wind_desc();
    m_wind_state   = {};
    m_wind_current = m_scene.wind();   // seed smoother with the base static vector

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
    std::vector<CpuInstanceData> cpu_instances;
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
    EcosystemDesc& eco = m_scene.ecosystem();
    if (!eco.enabled || eco.species.empty())
    {
        m_ecosystem_placed = false;
        m_ecosystem_dirty  = false;
        return;
    }

    MARS_LOG("[Renderer] setup_ecosystem: {} species, max_instances={}, density='{}'",
             eco.species.size(), eco.max_instances, eco.density_map_path);

    // ---- Density map ---------------------------------------------------------
    if (!eco.density_map_path.empty())
    {
        uint32_t slot = m_resource_mgr.load_texture(m_device_ctx, eco.density_map_path,
                                                    /*is_srgb=*/false);
        if (slot != UINT32_MAX)
            eco.density_map_srv = slot;
        else
            MARS_LOG("[Renderer] setup_ecosystem: failed to load density map '{}'",
                     eco.density_map_path);
    }

    // ---- Species assets + per-LOD BLAS --------------------------------------
    namespace fs = std::filesystem;
    auto find_first_fbx = [](const fs::path& dir) -> std::string
    {
        if (!fs::exists(dir) || !fs::is_directory(dir)) return {};
        for (const auto& e : fs::directory_iterator(dir))
            if (e.is_regular_file() && e.path().extension() == ".fbx")
                return e.path().string();
        return {};
    };

    for (auto& sp : eco.species)
    {
        if (sp.asset_path.empty())
            continue;

        const fs::path base = sp.asset_path;
        const std::string high_fbx = find_first_fbx(base / "HighPoly");
        const std::string low_fbx  = find_first_fbx(base / "LowPoly");

        const std::string lod_paths[3] = { high_fbx, low_fbx, low_fbx }; // FarCluster reuses LowPoly

        for (uint32_t li = 0; li < 3u; ++li)
        {
            if (lod_paths[li].empty()) continue;
            // Vegetation FBXs (SpeedTree ORCA) consist of many independently-
            // placed submeshes (trunk / branches / leaves / fronds) whose final
            // positions are encoded in per-node transforms, and they are exported
            // in centimetres. Bake both into the vertex positions so the BLAS is
            // built in a single, world-scale, flat coordinate space.
            uint32_t mi = m_resource_mgr.load_model(m_device_ctx, lod_paths[li],
                                                    /*pre_transform_vertices=*/true);
            if (mi == UINT32_MAX) continue;
            sp.model_indices[li] = mi;

            const GpuModel& m = m_resource_mgr.model(mi);
            if (m.mesh_buffers.empty()) continue;

            // Build one refittable BLAS per submesh. SpeedTree FBX assets split a
            // tree into many submeshes (bark / branches / leaves / fronds),
            // each with its own material; registering one TLAS instance per
            // submesh later lets each one shade with the correct textures.
            // Each submesh gets an output vertex buffer (UAV) so the per-frame
            // wind compute pass can deform it and refit the BLAS each frame.
            //
            // Leaf / frond submeshes are flagged as non-opaque so the path
            // tracer's any-hit shader is invoked and can perform per-texel
            // alpha-cutout rejection. Trunk/branch submeshes stay opaque to
            // keep the BVH traversal fast on the bulk of the geometry.
            sp.blas_indices[li].clear();
            sp.blas_indices[li].reserve(m.mesh_buffers.size());
            for (size_t mi_idx = 0; mi_idx < m.mesh_buffers.size(); ++mi_idx)
            {
                auto& mb = m_resource_mgr.model(mi).mesh_buffers[mi_idx];
                // Allocate wind deformation output buffer (UAV + SRV).
                // This seeds the buffer with rest-pose data and registers
                // the bindless slots used by dispatch_vegetation_wind.
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

        // Impostor LOD: build a procedural-AABB BLAS using the species' bounds.
        if (sp.model_indices[0] != UINT32_MAX)
        {
            const GpuModel& m0 = m_resource_mgr.model(sp.model_indices[0]);
            const auto i_idx = static_cast<size_t>(VegetationLOD::Impostor);
            sp.blas_indices[i_idx].clear();
            sp.blas_indices[i_idx].push_back(
                m_path_tracer.build_vegetation_impostor_blas(
                    m_device_ctx, m0.bounds.min_pt, m0.bounds.max_pt));
        }

        MARS_LOG("[Renderer]   Species '{}': submeshes/LOD = [{},{},{},{}]",
                 sp.name,
                 sp.blas_indices[0].size(), sp.blas_indices[1].size(),
                 sp.blas_indices[2].size(), sp.blas_indices[3].size());
    }

    // ---- GPU resources: instance buffer, atomic counter, species table -----
    // Pack per-species LOD distance thresholds (must match SpeciesGpu in
    // vegetation_lod_selection.hlsl: 4 floats per species).
    const uint32_t species_count = static_cast<uint32_t>(eco.species.size());
    std::vector<float> species_table(static_cast<size_t>(species_count) * 4u);
    for (uint32_t i = 0; i < species_count; ++i)
    {
        species_table[i * 4 + 0] = eco.species[i].lod_near_max;
        species_table[i * 4 + 1] = eco.species[i].lod_mid_max;
        species_table[i * 4 + 2] = eco.species[i].lod_far_max;
        species_table[i * 4 + 3] = eco.species[i].max_draw_distance;
    }

    m_ecosystem_gpu.destroy();
    m_ecosystem_gpu.create(m_device_ctx,
                           m_resource_mgr.allocator(),
                           eco.max_instances,
                           species_count,
                           species_table.data());

    eco.instance_buffer_uav  = m_ecosystem_gpu.instance_uav();
    eco.instance_counter_uav = m_ecosystem_gpu.counter_uav();
    eco.species_buffer_srv   = m_ecosystem_gpu.species_srv();

    MARS_LOG("[Renderer] setup_ecosystem: GPU buffers ready (instance_uav={}, counter_uav={}, species_srv={})",
             eco.instance_buffer_uav, eco.instance_counter_uav, eco.species_buffer_srv);

    m_ecosystem_placed = false;
    m_ecosystem_dirty  = false;
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
    EcosystemDesc& eco = m_scene.ecosystem();
    if (!eco.enabled || eco.species.empty())
        return;

    if (eco.instance_buffer_uav  == UINT32_MAX ||
        eco.instance_counter_uav == UINT32_MAX ||
        eco.species_buffer_srv   == UINT32_MAX ||
        eco.density_map_srv      == UINT32_MAX)
    {
        MARS_LOG("[Renderer] place_and_register_vegetation: ecosystem resources not ready, skipping");
        return;
    }

    ID3D12Resource* counter_default  = m_ecosystem_gpu.counter_resource();
    ID3D12Resource* instance_default = m_ecosystem_gpu.instance_resource();
    ID3D12Resource* counter_readback = m_ecosystem_gpu.counter_readback();
    ID3D12Resource* instance_readback= m_ecosystem_gpu.instance_readback();
    if (!counter_default || !instance_default || !counter_readback || !instance_readback)
        return;

    // Bind the bindless heap so the placement shader can write through it.
    ID3D12DescriptorHeap* heaps[] = { m_device_ctx.bindless_heap() };
    m_cmd_list->SetDescriptorHeaps(1, heaps);

    // Counter is in COMMON; dispatch writes via UAV — Common -> UAV is implicit
    // for buffers, but barrier explicitly to satisfy the validation layer.
    D3D12_RESOURCE_BARRIER pre[2]{};
    pre[0] = CD3DX12_RESOURCE_BARRIER::Transition(counter_default,
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pre[1] = CD3DX12_RESOURCE_BARRIER::Transition(instance_default,
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_cmd_list->ResourceBarrier(2, pre);

    // Dispatch placement
    m_path_tracer.dispatch_vegetation_placement(m_cmd_list.Get(), eco);

    // UAV -> CopySource for readback
    D3D12_RESOURCE_BARRIER post[2]{};
    post[0] = CD3DX12_RESOURCE_BARRIER::Transition(counter_default,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    post[1] = CD3DX12_RESOURCE_BARRIER::Transition(instance_default,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_cmd_list->ResourceBarrier(2, post);

    m_cmd_list->CopyResource(counter_readback, counter_default);

    const uint64_t instance_bytes = static_cast<uint64_t>(eco.max_instances) * 64ull;
    m_cmd_list->CopyBufferRegion(instance_readback, 0, instance_default, 0, instance_bytes);

    // Restore the default-heap resources to COMMON so subsequent passes
    // (LOD selection / wind) can transition them as needed.
    D3D12_RESOURCE_BARRIER end[2]{};
    end[0] = CD3DX12_RESOURCE_BARRIER::Transition(counter_default,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    end[1] = CD3DX12_RESOURCE_BARRIER::Transition(instance_default,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    m_cmd_list->ResourceBarrier(2, end);

    // Flush so the readback is CPU-visible.
    throw_if_failed(m_cmd_list->Close(), "CmdList::Close (place_vegetation)");
    ID3D12CommandList* lists[] = { m_cmd_list.Get() };
    m_device_ctx.direct_queue()->ExecuteCommandLists(1, lists);
    m_device_ctx.flush_gpu();

    // Re-open the command list so the caller can continue recording (rebuild_tlas).
    throw_if_failed(m_cmd_allocators[0]->Reset(), "CmdAlloc::Reset (place_vegetation)");
    throw_if_failed(m_cmd_list->Reset(m_cmd_allocators[0].Get(), nullptr),
                    "CmdList::Reset (place_vegetation)");

    // Read back placement results.
    const uint32_t placed_count_raw = m_ecosystem_gpu.read_counter();
    const uint32_t placed_count     = std::min(placed_count_raw, eco.max_instances);
    eco.instance_count = placed_count;

    MARS_LOG("[Renderer] place_and_register_vegetation: GPU reported {} placed instances (clamped to {})",
             placed_count_raw, placed_count);

    if (placed_count == 0)
        return;

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

    std::vector<VegetationInstanceGpu> instances(placed_count);
    m_ecosystem_gpu.read_instances(instances.data(), placed_count);

    // Reserve runtime CPU-side vegetation instance table so dispatch_ecosystem
    // and later systems can iterate them if needed.
    eco.instances.clear();
    eco.instances.reserve(placed_count);

    // ---- Build per-species per-LOD per-submesh material slots ---------------
    // SpeedTree FBXs split a tree into many submeshes (bark / branches /
    // leaves / fronds), each with its own material. We allocate one
    // CpuMaterialData per (species,lod,submesh) and remember the slot in
    // SpeciesDesc::material_indices so all instances of the same species
    // share them (cheap; one slot per submesh, not per tree).
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
                // Default to opaque double-sided (foliage is double-sided).
                // The alpha-masked bit is set per-submesh below from the
                // imported MaterialData::alpha_masked flag — SpeedTree FBX
                // exports mark leaf/frond cards via material/texture naming,
                // which the importer's name heuristic detects.
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
                    if (src.alpha_masked) mat.flags |= 0x2u; // alpha-tested foliage card
                }

                sp.material_indices[lod][s] = static_cast<uint32_t>(cpu_materials.size());
                cpu_materials.push_back(mat);
            }
        }
    };

    for (uint32_t i = 0; i < placed_count; ++i)
    {
        const VegetationInstanceGpu& g = instances[i];
        if (g.species_index >= eco.species.size())
            continue;

        SpeciesDesc& sp = eco.species[g.species_index];
        ensure_species_materials(sp);

        // Initial LOD = Near; per-frame compute pass updates it later.
        const auto lod_idx = static_cast<size_t>(VegetationLOD::Near);
        if (sp.blas_indices[lod_idx].empty())
            continue;
        if (sp.model_indices[lod_idx] == UINT32_MAX)
            continue;

        // Build a world transform from position+scale+rotation.
        const Vec3       pos   { g.position_scale[0], g.position_scale[1], g.position_scale[2] };
        const float      scale = g.position_scale[3];
        const Quaternion rot   { g.rotation[0], g.rotation[1], g.rotation[2], g.rotation[3] };

        Transform xf;
        xf.position = pos;
        xf.rotation = rot;
        xf.scale    = scale;
        const Mat4x4 world = xf.to_matrix();

        const GpuModel& m0 = m_resource_mgr.model(sp.model_indices[lod_idx]);

        // Register one TLAS instance per submesh so each gets its own
        // BLAS, vertex/index SRVs and material (textures).
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
            // Use the wind-deformed (skinned output) vertex buffer if one was allocated so
            // that FetchInterpolatedVertex reads deformed positions/normals/UVs, keeping
            // shadow ray origins and surface normals consistent with the actual BLAS geometry.
            // Falls back to the rest-pose buffer for non-wind LODs (e.g., Impostor).
            inst_data.vertex_buffer_srv = (m0.mesh_buffers[s].skinned_vertex_srv_slot() != UINT32_MAX)
                                          ? m0.mesh_buffers[s].skinned_vertex_srv_slot()
                                          : m0.mesh_buffers[s].vertex_srv_slot();
            inst_data.index_buffer_srv  = m0.mesh_buffers[s].index_srv_slot();
            // Supply the compact float3 prev-position buffer so the closest-hit shader
            // can compute per-vertex motion vectors for denoiser / TAA reprojection.
            // Without this the denoiser sees zero object motion and produces blur/ghosting
            // on wind-animated trees. The wind compute shader writes last frame's deformed
            // positions here before computing the new positions each frame.
            inst_data.prev_vertex_buffer_srv = m0.mesh_buffers[s].wind_prev_pos_srv_slot();
            cpu_instances.push_back(inst_data);

            m_path_tracer.set_instance(tlas_instance, blas_idx, world, mat_idx);
            ++tlas_instance;
        }

        // Record runtime vegetation entry (track the first TLAS slot).
        VegetationInstance vi{};
        vi.transform         = xf;
        vi.species_index     = g.species_index;
        vi.current_lod       = VegetationLOD::Near;
        vi.tlas_instance     = first_tlas_for_inst;
        vi.wind_phase_offset = g.wind_phase_offset;
        eco.instances.push_back(vi);
    }

    m_ecosystem_placed = true;
    m_ecosystem_dirty  = false;

    // Ensure per-species spring oscillators are sized to match (reset to zero).
    m_species_wind_states.assign(eco.species.size(), SpeciesWindState{});

    MARS_LOG("[Renderer] place_and_register_vegetation: registered {} vegetation TLAS instances (total now {})",
             eco.instances.size(), tlas_instance);

    (void)cpu_materials;  // unused for now; future per-species materials may need it
}

// ---------------------------------------------------------------------------
// dispatch_ecosystem (M10 step-10)
//
// Per-frame ecosystem GPU work. Runs vegetation placement once (when
// instance/counter/species GPU resources are valid), and vegetation LOD
// selection every frame thereafter. Wind deformation + BLAS refit per LOD
// will be wired in once the per-species output vertex buffers are allocated.
// ---------------------------------------------------------------------------
void Renderer::dispatch_ecosystem()
{
    EcosystemDesc& eco = m_scene.ecosystem();
    if (!eco.enabled || eco.species.empty())
        return;

    // Required GPU resources for placement / LOD selection. If any are still
    // unset, ecosystem setup hasn't completed — skip silently.
    if (eco.instance_buffer_uav  == UINT32_MAX ||
        eco.instance_counter_uav == UINT32_MAX ||
        eco.species_buffer_srv   == UINT32_MAX ||
        eco.density_map_srv      == UINT32_MAX)
    {
        return;
    }

    // One-shot placement.
    if (!m_ecosystem_placed)
    {
        // Placement is now performed synchronously at scene-load time inside
        // place_and_register_vegetation(); if for some reason it did not run
        // we still mark the flag here so we don't keep retrying.
        m_ecosystem_placed = true;
        m_ecosystem_dirty  = true;
    }

    // Per-frame LOD selection. Uses the primary output's camera.
    if (eco.instance_count > 0 && !m_cameras.empty())
    {
        m_path_tracer.dispatch_vegetation_lod_selection(
            m_cmd_list.Get(), eco, m_cameras[0].position, m_frame_index);
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

    // ---- Animated wind evaluation ------------------------------------------
    // Advance phase accumulators and combine the three oscillator layers to
    // produce an IIR-smoothed instantaneous wind vector for the cloth solver.
    {
        const WindDesc& wd  = m_wind_desc;
        const float     dt2 = m_last_delta_time;

        // Two-pi factors for each oscillator.
        const float tau = 6.28318530718f;

        // Advance phases.
        m_wind_state.gust_phase    += tau * wd.gust_frequency             * dt2;
        m_wind_state.wander_phase  += tau * wd.direction_wander_frequency * dt2;
        m_wind_state.micro_phase_x += tau * wd.micro_frequency            * dt2;
        m_wind_state.micro_phase_z += tau * wd.micro_frequency * 1.37f    * dt2; // decouple X/Z

        // Wrap phases to avoid float precision loss over time.
        auto wrap = [&](float& p) { if (p > tau * 1000.0f) p -= tau * 1000.0f; };
        wrap(m_wind_state.gust_phase);
        wrap(m_wind_state.wander_phase);
        wrap(m_wind_state.micro_phase_x);
        wrap(m_wind_state.micro_phase_z);

        // 1. Gust: modulate speed.
        float gust_speed = wd.base_speed * (1.0f + wd.gust_strength * std::sin(m_wind_state.gust_phase));

        // 2. Direction wander: rotate base_direction in XZ plane by a small angle.
        float wander_rad  = (wd.direction_wander_angle * 0.01745329252f)   // deg→rad
                            * std::sin(m_wind_state.wander_phase);
        float cos_w = std::cos(wander_rad);
        float sin_w = std::sin(wander_rad);
        Vec3  wander_dir = {
            wd.base_direction.x * cos_w - wd.base_direction.z * sin_w,
            wd.base_direction.y,
            wd.base_direction.x * sin_w + wd.base_direction.z * cos_w
        };

        // 3. Assemble pre-micro vector.
        Vec3 target = {
            wander_dir.x * gust_speed,
            wander_dir.y * gust_speed,
            wander_dir.z * gust_speed
        };

        // 4. Micro turbulence: add small per-axis sinusoidal offsets.
        float micro_amp = wd.micro_variation * gust_speed;
        target.x += micro_amp * std::sin(m_wind_state.micro_phase_x);
        target.z += micro_amp * std::sin(m_wind_state.micro_phase_z);

        // 5. Frame-rate-independent one-pole IIR smoother.
        //    s_base is the per-frame coefficient at the reference rate of 60 fps.
        //    The equivalent continuous time-constant is: tau = -1 / (60 * ln(s_base)).
        //    We then compute the actual per-frame alpha from the real dt so the
        //    feel is identical at any frame rate.
        {
            const float s_base = std::max(0.0f, std::min(wd.response_smoothing, 0.9999f));
            float alpha;
            if (s_base < 1e-4f)
            {
                alpha = 1.0f; // instant (no smoothing requested)
            }
            else
            {
                // tau_s [seconds] — derived from 60fps reference; independent of dt.
                const float tau_s = -1.0f / (60.0f * std::log(s_base));
                alpha = 1.0f - std::exp(-dt2 / std::max(tau_s, 1e-4f));
            }
            m_wind_current.x += alpha * (target.x - m_wind_current.x);
            m_wind_current.y += alpha * (target.y - m_wind_current.y);
            m_wind_current.z += alpha * (target.z - m_wind_current.z);
        }
    }

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
    // render_frame_path_traced() using the evaluated m_wind_current and delta_time.
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
            m_path_tracer.dispatch_cloth_sim(m_cmd_list.Get(), ci, m_wind_current, k_cloth_substep_dt);
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
    bool any_vegetation_wind = false;
    {
        EcosystemDesc& eco = m_scene.ecosystem();
        if (eco.enabled && m_ecosystem_placed)
        {
            const Vec3  wind_dir      = [&]{ Vec3 d = m_wind_current;
                                              float len = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
                                              return len > 1e-4f ? Vec3{d.x/len, d.y/len, d.z/len}
                                                                 : Vec3{1.f,0.f,0.f}; }();
            const float wind_strength = std::sqrt(m_wind_current.x*m_wind_current.x +
                                                   m_wind_current.y*m_wind_current.y +
                                                   m_wind_current.z*m_wind_current.z);

            // Only deform LODs that have placed instances (Near/Mid/FarCluster).
            constexpr VegetationLOD k_wind_lods[] = {
                VegetationLOD::Near, VegetationLOD::Mid, VegetationLOD::FarCluster };

            const float spring_dt = m_last_delta_time;

            for (size_t sp_idx = 0; sp_idx < eco.species.size(); ++sp_idx)
            {
                SpeciesDesc& sp = eco.species[sp_idx];

                // ---------------------------------------------------------------
                // Per-species damped spring oscillator
                //
                // Models the trunk as a second-order damped harmonic oscillator
                // driven by the normalised wind speed (wind_t_target ∈ [0,1]):
                //
                //   ẍ + 2ζω₀ẋ + ω₀²x = ω₀² · wind_t_target
                //
                //   ω₀   = 2π × 0.22 Hz  →  ~4.5 s natural period
                //   ζ    = 0.28           →  lightly underdamped (1-2 overshoots)
                //
                // Result: trunk_envelope builds up over ~3-4 s, overshoots
                // slightly on gusts, and decays slowly when wind drops.
                // This is the only layer that uses the spring; branches and
                // leaves use wind_t_raw (instant) for hierarchical contrast.
                //
                // Per-species phase offset: a deterministic fractional offset
                // derived from the species index ensures different species are
                // out of phase and never animate in synchrony.
                // ---------------------------------------------------------------
                const float wind_t_target = std::min(wind_strength / 20.0f, 1.0f);

                // Pseudorandom per-species phase offset in [0, 2π).
                // Uses a simple integer hash (golden-ratio mixing) so each
                // species index maps to a unique, well-distributed offset.
                const uint32_t hash_val    = static_cast<uint32_t>(sp_idx) * 2654435761u;
                const float    sp_phase    = static_cast<float>(hash_val >> 8) / static_cast<float>(1u << 24);
                // sp_phase ∈ [0, 1) → convert to radians in dispatch call

                if (sp_idx < m_species_wind_states.size())
                {
                    SpeciesWindState& ws = m_species_wind_states[sp_idx];

                    // Natural frequency: 0.22 Hz → ~4.5 s period, matching a large tree's
                    // fundamental sway mode.  Damping ratio 0.28 → lightly underdamped:
                    // ~1-2 gentle overshoots before settling, ~3-4 s build-up / decay.
                    constexpr float k_w0      = 6.28318f * 0.22f; // rad/s  (0.22 Hz)
                    constexpr float k_zeta    = 0.28f;             // damping ratio
                    constexpr float k_2zw0    = 2.0f * k_zeta * k_w0;
                    constexpr float k_w0sq    = k_w0 * k_w0;

                    // Semi-implicit Euler integration (stable even at large dt).
                    const float accel = k_w0sq * (wind_t_target - ws.pos) - k_2zw0 * ws.vel;
                    ws.vel += accel  * spring_dt;
                    ws.pos += ws.vel * spring_dt;
                    ws.pos  = std::max(0.0f, ws.pos);  // can't push below zero
                }

                const float trunk_envelope = (sp_idx < m_species_wind_states.size())
                                             ? std::min(m_species_wind_states[sp_idx].pos, 1.5f)
                                             : wind_t_target;

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

                        // Mesh bounds for height-based per-vertex weighting.
                        const float mesh_min_y  = gm.bounds.min_pt.y;
                        const float mesh_height = gm.bounds.max_pt.y - gm.bounds.min_pt.y;

                        m_path_tracer.dispatch_vegetation_wind(
                            m_cmd_list.Get(),
                            mb.vertex_count(),
                            mb.vertex_srv_slot(),
                            mb.skinned_vertex_uav_slot(),
                            mb.wind_prev_pos_uav_slot(),
                            mesh_min_y,
                            mesh_height,
                            wind_dir,
                            wind_strength,
                            m_elapsed_time_seconds,
                            sp_phase * 6.28318f,  // per-species pseudorandom phase offset
                            sp.wind_primary_bend,
                            sp.wind_secondary_sway,
                            sp.wind_leaf_flutter,
                            trunk_envelope);

                        // UAV barrier: ensure wind output is visible to BLAS refit.
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

    // Rebuild TLAS once if any BLAS was refitted this frame.
    if (any_skinned || m_rigid_nodes_dirty || m_cloth_dirty || any_vegetation_wind)
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
                    b.Transition.StateBefore     = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
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

} // namespace mars
