// =============================================================================
// renderer.cpp
// MARS 3D Engine — Renderer implementation (M5: scene loading + static scene)
// =============================================================================

#include "mars_engine/renderer/renderer.h"
#include "mars_engine/scene/scene_loader.h"

#include <stdexcept>
#include <format>
#include <print>

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
    m_scene.unload();
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
        std::println("[Renderer] load_scene() called before init().");
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

    std::println("[Renderer] load_scene: {} scene instance(s), {} model(s) in resource manager.",
                 m_scene.instances().size(), m_resource_mgr.model_count());

    for (const auto& inst : m_scene.instances())
    {
        if (inst.model_index == UINT32_MAX) continue;

        // Skip if this model's BLAS has already been built (shared by multiple instances).
        if (blas_base_index[inst.model_index] != UINT32_MAX) continue;

        const GpuModel& model = m_resource_mgr.model(inst.model_index);
        std::println("[Renderer]   Building BLAS for model_index={} mesh_count={}",
                     inst.model_index, model.mesh_buffers.size());

        for (uint32_t mi = 0; mi < static_cast<uint32_t>(model.mesh_buffers.size()); ++mi)
        {
            std::println("[Renderer]     mesh[{}]: {} verts, {} idx",
                         mi,
                         model.mesh_buffers[mi].vertex_count(),
                         model.mesh_buffers[mi].index_count());

            uint32_t blas_idx = m_path_tracer.build_blas(
                m_device_ctx, model.mesh_buffers[mi]);

            std::println("[Renderer]     -> BLAS index {}", blas_idx);

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

    for (const auto& inst : m_scene.instances())
    {
        if (inst.model_index == UINT32_MAX) continue;

        const GpuModel& model = m_resource_mgr.model(inst.model_index);
        Mat4x4 world = inst.transform.to_matrix();

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
            inst_data.vertex_buffer_srv = model.mesh_buffers[mi].vertex_srv_slot();
            inst_data.index_buffer_srv  = model.mesh_buffers[mi].index_srv_slot();
            cpu_instances.push_back(inst_data);

            std::println("[Renderer]   TLAS instance {}: blas={} mat={} vtx_srv={} idx_srv={} ('{}')",
                         tlas_instance, blas_idx, mat_idx,
                         inst_data.vertex_buffer_srv, inst_data.index_buffer_srv,
                         inst.name);
            m_path_tracer.set_instance(tlas_instance++, blas_idx, world, mat_idx);
        }
    }

    std::println("[Renderer] Total TLAS instances to build: {}", tlas_instance);

    // Build the TLAS and flush.
    throw_if_failed(m_cmd_allocators[0]->Reset(), "CmdAlloc::Reset (load_scene)");
    throw_if_failed(m_cmd_list->Reset(m_cmd_allocators[0].Get(), nullptr),
                    "CmdList::Reset (load_scene)");

    rebuild_tlas();

    throw_if_failed(m_cmd_list->Close(), "CmdList::Close (load_scene)");
    ID3D12CommandList* lists[] = { m_cmd_list.Get() };
    m_device_ctx.direct_queue()->ExecuteCommandLists(1, lists);
    m_device_ctx.flush_gpu();

    // Upload per-instance and per-material structured buffers to the GPU.
    m_path_tracer.upload_scene_buffers(m_device_ctx, cpu_instances, cpu_materials);

    std::println("[Renderer] Scene '{}' loaded: {} TLAS instance(s).",
                 m_scene.name(), tlas_instance);
    return true;
}

// ---------------------------------------------------------------------------
// rebuild_tlas
// ---------------------------------------------------------------------------
void Renderer::rebuild_tlas()
{
    m_path_tracer.build_tlas(m_device_ctx, m_cmd_list.Get());
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
        std::println("[Renderer] render_frame() path: {}",
                     path_traced ? "PATH_TRACED" : "CLEAR_COLOR_FALLBACK");
        std::println("[Renderer]   path_tracer.is_initialised() = {}",
                     m_path_tracer.is_initialised() ? "true" : "false");
        std::println("[Renderer]   path_tracer.tlas_srv_slot()  = {}",
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

    wait_for_frame(back_index);

    throw_if_failed(m_cmd_allocators[back_index]->Reset(), "CommandAllocator::Reset failed");
    throw_if_failed(m_cmd_list->Reset(m_cmd_allocators[back_index].Get(), nullptr),
                    "CommandList::Reset failed");

    // Bind the bindless heap so DXR shaders can access all resources.
    ID3D12DescriptorHeap* heaps[] = { m_device_ctx.bindless_heap() };
    m_cmd_list->SetDescriptorHeaps(1, heaps);

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
