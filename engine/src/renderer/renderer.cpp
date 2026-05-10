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

        // Prime resize for all outputs
        for (uint32_t i = 0; i < output_count; ++i)
        {
            DisplayOutput& disp = m_display_manager.output(i);
            m_path_tracer.resize_output(m_device_ctx, i, disp.width(), disp.height());
        }
    }

    // Initialise per-output camera state list
    m_cameras.resize(output_count);

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
    if (m_path_tracer.is_initialised())
        m_path_tracer.resize_output(m_device_ctx, output_index, width, height);
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

    m_cameras[output_index] = { position, view_inv, proj_inv };
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

        const GpuModel& model = m_resource_mgr.model(inst.model_index);
        std::println("[Renderer]   Instance '{}': model_index={}, mesh_count={}",
                     inst.name, inst.model_index, model.mesh_buffers.size());

        for (uint32_t mi = 0; mi < static_cast<uint32_t>(model.mesh_buffers.size()); ++mi)
        {
            // Build BLAS once per mesh_buffer (model_index + mesh_index key).
            uint32_t key_index = inst.model_index * 1024u + mi; // simple unique key
            if (blas_base_index[inst.model_index] == UINT32_MAX || mi > 0)
            {
                std::println("[Renderer]     Building BLAS for model[{}] mesh[{}] ({} verts, {} idx)",
                             inst.model_index, mi,
                             model.mesh_buffers[mi].vertex_count(),
                             model.mesh_buffers[mi].index_count());

                uint32_t blas_idx = m_path_tracer.build_blas(
                    m_device_ctx, model.mesh_buffers[mi]);

                std::println("[Renderer]     -> BLAS index {}", blas_idx);

                if (mi == 0)
                    blas_base_index[inst.model_index] = blas_idx;

                (void)key_index; // used conceptually above
            }
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

            // Material: use a per-mesh material entry (white for now; extend when
            // proper material data is plumbed through ResourceManager)
            uint32_t mat_idx = static_cast<uint32_t>(cpu_materials.size());
            {
                CpuMaterialData mat{};
                mat.base_color_factor[0] = 0.8f;
                mat.base_color_factor[1] = 0.8f;
                mat.base_color_factor[2] = 0.8f;
                mat.base_color_factor[3] = 1.0f;
                mat.roughness_factor = 0.8f;
                // If the model has a bound texture for this mesh, wire it in
                if (mi < model.texture_slots.size() && model.texture_slots[mi] != UINT32_MAX)
                    mat.base_color_tex = model.texture_slots[mi];
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
        // Update per-frame constants
        const CameraState& cam = (oi < m_cameras.size()) ? m_cameras[oi] : m_cameras[0];
        m_path_tracer.update_frame_constants(m_device_ctx, oi, m_frame_index,
                                             cam.position, cam.view_inv, cam.proj_inv);

        // Trace rays → UAV
        m_path_tracer.trace(m_cmd_list.Get(), oi);

        // UAV barrier between trace and copy
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = nullptr; // all UAVs
        m_cmd_list->ResourceBarrier(1, &uav);

        // Transition back buffer: PRESENT → RENDER_TARGET
        DisplayOutput& display_out = m_display_manager.output(oi);
        display_out.transition(m_cmd_list.Get(),
                       D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);

        // Blit UAV → back buffer with tone-mapping
        m_path_tracer.copy_to_back_buffer(m_cmd_list.Get(), oi,
                                           display_out.current_back_buffer(),
                                           display_out.current_rtv(),
                                           static_cast<uint32_t>(display_out.hdr_mode()),
                                           display_out.back_buffer_format());

        // Transition back buffer: RENDER_TARGET → PRESENT
        display_out.transition(m_cmd_list.Get(),
                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PRESENT);
    }

    throw_if_failed(m_cmd_list->Close(), "CommandList::Close failed");

    ID3D12CommandList* lists[] = { m_cmd_list.Get() };
    m_device_ctx.direct_queue()->ExecuteCommandLists(1, lists);

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
