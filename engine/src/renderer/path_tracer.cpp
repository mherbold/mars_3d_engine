// =============================================================================
// path_tracer.cpp
// MARS 3D Engine — DXR path-tracing pipeline implementation
// =============================================================================

#include "mars_engine/engine_api.h"
#include "mars_engine/renderer/path_tracer.h"
#include "mars_engine/renderer/device_context.h"
#include "mars_engine/scene/scene_types.h"

#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <vector>
#include <string>
#include <windows.h>
#include <cassert>
#include <bit>

// D3D12 Memory Allocator
#pragma warning(push, 0)
#include "D3D12MemAlloc.h"
#pragma warning(pop)

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

// Load a compiled DXIL blob from disk (produced by the DXC shader build step).
// The file is expected next to the executable in a `dxil/` sub-directory.
static std::vector<uint8_t> load_dxil_blob(const std::wstring& filename)
{
    // Try relative to the executable directory first, then the working directory.
    wchar_t exe_path_buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path_buf, MAX_PATH);

    std::wstring exe_dir(exe_path_buf);
    auto slash = exe_dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        exe_dir = exe_dir.substr(0, slash + 1);

    auto try_load = [](const std::wstring& path) -> std::vector<uint8_t>
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return {};
        auto size = f.tellg();
        f.seekg(0);
        std::vector<uint8_t> data(static_cast<size_t>(size));
        f.read(reinterpret_cast<char*>(data.data()), size);
        return data;
    };

    std::wstring path1 = exe_dir + L"dxil\\" + filename;
    std::wstring path2 = std::wstring(L"dxil\\") + filename;

    // Convert wstring to string for logging (ASCII paths only — DXIL filenames are safe).
    auto wstr_to_str = [](const std::wstring& ws) -> std::string
    {
        std::string s(ws.size(), '\0');
        for (size_t i = 0; i < ws.size(); ++i)
            s[i] = static_cast<char>(ws[i]);
        return s;
    };

    MARS_LOG("[PathTracer] Searching for DXIL blob:");
    MARS_LOG("[PathTracer]   Try 1: '{}'", wstr_to_str(path1));

    auto blob = try_load(path1);
    if (blob.empty())
    {
        MARS_LOG("[PathTracer]   Try 1 FAILED — not found or empty.");
        MARS_LOG("[PathTracer]   Try 2: '{}'", wstr_to_str(path2));
        blob = try_load(path2);
        if (blob.empty())
            MARS_LOG("[PathTracer]   Try 2 FAILED — not found or empty.");
        else
            MARS_LOG("[PathTracer]   Try 2 OK — {} bytes loaded.", blob.size());
    }
    else
    {
        MARS_LOG("[PathTracer]   Try 1 OK — {} bytes loaded.", blob.size());
    }

    if (blob.empty())
        throw std::runtime_error(
            "PathTracer: could not load DXIL blob.\n"
            "Ensure the 'shaders_dxil' CMake target has been built and "
            "the dxil/ directory has been copied next to the executable.");

    return blob;
}

// Round up to the nearest multiple of `align`.
static constexpr uint64_t align_up(uint64_t value, uint64_t align)
{
    return (value + align - 1) & ~(align - 1);
}

// ---------------------------------------------------------------------------
// PathTracer-owned D3D12MA allocator
// ---------------------------------------------------------------------------
void PathTracer::create_allocator(DeviceContext& ctx)
{
    D3D12MA::ALLOCATOR_DESC desc{};
    desc.pDevice  = ctx.device();
    desc.pAdapter = ctx.adapter();
    desc.Flags    = D3D12MA::ALLOCATOR_FLAG_NONE;

    throw_if_failed(
        D3D12MA::CreateAllocator(&desc, &m_allocator),
        "PathTracer: D3D12MA::CreateAllocator failed");
}

// ---------------------------------------------------------------------------
// Instance helper: create a D3D12MA upload buffer and map it.
// ---------------------------------------------------------------------------
void PathTracer::create_upload_buffer(DeviceContext& ctx,
                                      uint64_t size, const wchar_t* name,
                                      D3D12MA::Allocation** out_alloc,
                                      ID3D12Resource** out_resource,
                                      void** out_mapped_ptr)
{
    D3D12MA::ALLOCATION_DESC alloc_desc{};
    alloc_desc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC buf_desc{};
    buf_desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width            = align_up(size, 256);
    buf_desc.Height           = 1;
    buf_desc.DepthOrArraySize = 1;
    buf_desc.MipLevels        = 1;
    buf_desc.SampleDesc.Count = 1;
    buf_desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    throw_if_failed(
        m_allocator->CreateResource(
            &alloc_desc,
            &buf_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            out_alloc,
            IID_PPV_ARGS(out_resource)),
        "D3D12MA: create upload buffer failed");

    (*out_resource)->SetName(name);
    (*out_resource)->Map(0, nullptr, out_mapped_ptr);
    (void)ctx;
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
void PathTracer::init(DeviceContext& ctx,
                      uint32_t output_width, uint32_t output_height,
                      uint32_t output_count)
{
    if (m_initialised) return;

    m_output_count = output_count;

    create_allocator(ctx);
    create_rtpso(ctx);
    create_blit_pipeline(ctx);
    create_skinning_pipeline(ctx);
    create_cloth_pipeline(ctx);
    create_vegetation_placement_pipeline(ctx);
    create_vegetation_lod_pipeline(ctx);
    create_vegetation_wind_pipeline(ctx);
    create_shader_tables(ctx);
    create_output_textures(ctx);
    create_cb_ring(ctx);

    // Resize all output textures to the given dimensions
    for (uint32_t i = 0; i < output_count; ++i)
        resize_output(ctx, i, output_width, output_height);

    m_initialised = true;
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------
void PathTracer::shutdown()
{
    if (!m_initialised) return;

    release_output_textures();

    // Release constant buffer ring
    for (auto& cb : m_frame_cbs)
    {
        if (cb.resource) { cb.resource->Unmap(0, nullptr); cb.resource = nullptr; }
        if (cb.alloc)    { cb.alloc->Release(); cb.alloc = nullptr; }
    }
    m_frame_cbs.clear();

    // Release shader tables
    auto release_table = [](D3D12MA::Allocation*& alloc, ID3D12Resource*& res)
    {
        if (alloc) { alloc->Release(); alloc = nullptr; res = nullptr; }
    };
    release_table(m_raygen_table_alloc,   m_raygen_table);
    release_table(m_miss_table_alloc,     m_miss_table);
    release_table(m_hitgroup_table_alloc, m_hitgroup_table);

    // Release TLAS
    if (m_tlas_alloc)         { m_tlas_alloc->Release();         m_tlas_alloc         = nullptr; m_tlas_resource    = nullptr; }
    if (m_tlas_scratch_alloc) { m_tlas_scratch_alloc->Release(); m_tlas_scratch_alloc = nullptr; m_tlas_scratch     = nullptr; }
    if (m_instance_buf_alloc) { m_instance_buf_alloc->Release(); m_instance_buf_alloc = nullptr; m_instance_buffer  = nullptr; }

    // Release scene instance / material buffers
    if (m_instance_data_alloc) { m_instance_data_alloc->Release(); m_instance_data_alloc = nullptr; m_instance_data_buffer = nullptr; }
    if (m_material_data_alloc) { m_material_data_alloc->Release(); m_material_data_alloc = nullptr; m_material_data_buffer = nullptr; }
    m_instance_data_srv_slot = UINT32_MAX;
    m_material_data_srv_slot = UINT32_MAX;

    // Release BLAS list
    for (auto& b : m_blas_list)
    {
        if (b.scratch_alloc) { b.scratch_alloc->Release(); b.scratch_alloc = nullptr; b.scratch = nullptr; }
        if (b.alloc) { b.alloc->Release(); b.alloc = nullptr; b.resource = nullptr; }
    }
    m_blas_list.clear();

    m_rtpso.Reset();
    m_rtpso_props.Reset();
    m_global_root_sig.Reset();
    m_local_root_sig.Reset();

    m_blit_pso_sdr.Reset();
    m_blit_pso_hdr10.Reset();
    m_blit_pso_scrgb.Reset();
    m_blit_root_sig.Reset();

    m_skinning_pso.Reset();
    m_skinning_root_sig.Reset();

    if (m_allocator) { m_allocator->Release(); m_allocator = nullptr; }

    m_initialised = false;
}

// ---------------------------------------------------------------------------
// create_blit_pipeline
// Builds a root signature and 3 PSOs (SDR / HDR10 / scRGB) for the
// tone-map blit that converts the RGBA16F path-tracer output into the
// swap-chain back buffer format.
// ---------------------------------------------------------------------------
void PathTracer::create_blit_pipeline(DeviceContext& ctx)
{
    auto* device = ctx.device();

    // ---- 1. Root signature --------------------------------------------------
    // Param 0: 2 root constants (src_texture_slot, hdr_mode)          b0 space0
    // Param 1: descriptor table — same bindless SRV heap as DXR shaders
    {
        CD3DX12_ROOT_PARAMETER1 params[2]{};
        params[0].InitAsConstants(2, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);

        static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS k_vol =
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
            D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        CD3DX12_DESCRIPTOR_RANGE1 srv_range;
        srv_range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 0, k_vol, 0);
        params[1].InitAsDescriptorTable(1, &srv_range, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC point_sampler(1,
            D3D12_FILTER_MIN_MAG_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        // Also expose the linear sampler at s0 so bindless.hlsli macros compile.
        CD3DX12_STATIC_SAMPLER_DESC linear_sampler(0,
            D3D12_FILTER_ANISOTROPIC,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP);

        CD3DX12_STATIC_SAMPLER_DESC samplers[2] = { linear_sampler, point_sampler };

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc;
        rs_desc.Init_1_1(2, params, 2, samplers,
                         D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> rs_blob, err_blob;
        throw_if_failed(
            D3DX12SerializeVersionedRootSignature(&rs_desc,
                D3D_ROOT_SIGNATURE_VERSION_1_1, &rs_blob, &err_blob),
            "PathTracer: serialize blit root signature failed");

        throw_if_failed(
            device->CreateRootSignature(0,
                rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                IID_PPV_ARGS(&m_blit_root_sig)),
            "PathTracer: create blit root signature failed");

        m_blit_root_sig->SetName(L"MARS::PathTracer::BlitRootSig");
    }

    // ---- 2. Load VS and PS DXIL blobs ---------------------------------------
    auto vs_blob = load_dxil_blob(L"tone_map_blit_vs.dxil");
    auto ps_blob = load_dxil_blob(L"tone_map_blit_ps.dxil");

    D3D12_SHADER_BYTECODE vs_bc{ vs_blob.data(), vs_blob.size() };
    D3D12_SHADER_BYTECODE ps_bc{ ps_blob.data(), ps_blob.size() };

    // ---- 3. Build PSOs for each back-buffer format --------------------------
    auto make_pso = [&](DXGI_FORMAT rt_format) -> ComPtr<ID3D12PipelineState>
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = m_blit_root_sig.Get();
        pso_desc.VS             = vs_bc;
        pso_desc.PS             = ps_bc;

        pso_desc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable       = FALSE;

        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        pso_desc.SampleMask                = UINT_MAX;
        pso_desc.PrimitiveTopologyType     = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets          = 1;
        pso_desc.RTVFormats[0]             = rt_format;
        pso_desc.SampleDesc.Count          = 1;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;

        ComPtr<ID3D12PipelineState> pso;
        throw_if_failed(
            device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso)),
            "PathTracer: create blit PSO failed");
        return pso;
    };

    m_blit_pso_sdr   = make_pso(DXGI_FORMAT_R8G8B8A8_UNORM);
    m_blit_pso_hdr10 = make_pso(DXGI_FORMAT_R10G10B10A2_UNORM);
    m_blit_pso_scrgb = make_pso(DXGI_FORMAT_R16G16B16A16_FLOAT);

    // Cache the GPU start of the bindless heap so copy_to_back_buffer can use it
    // without needing a DeviceContext pointer.
    m_bindless_heap_gpu_start = ctx.bindless_heap()->GetGPUDescriptorHandleForHeapStart();
}

// ---------------------------------------------------------------------------
// create_skinning_pipeline
// Builds the compute root signature and PSO for the GPU skinning pass.
// Root signature layout:
//   b0  space0  : SkinningConstants (4 root constants)
//   Descriptor table: same bindless heap used everywhere (SRV + UAV)
// ---------------------------------------------------------------------------
void PathTracer::create_skinning_pipeline(DeviceContext& ctx)
{
    auto* device = ctx.device();

    // ---- Root signature ----------------------------------------------------
    {
        CD3DX12_ROOT_PARAMETER1 params[2]{};

        // 4 inline root constants (vertex_count, src_vertex_srv, bone_palette_srv, dst_vertex_uav)
        params[0].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

        static constexpr UINT k_unbounded = UINT_MAX;
        static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS k_volatile =
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
            D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

        CD3DX12_DESCRIPTOR_RANGE1 ranges[3]{};
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, k_unbounded, 0, 1, k_volatile, 0); // t0, space1 — g_Buffers[]
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, k_unbounded, 0, 2, k_volatile, 0); // t0, space2 — g_BonePalettes[]
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, k_unbounded, 0, 3, k_volatile, 0); // u0, space3 — g_OutputBuffers[]

        params[1].InitAsDescriptorTable(3, ranges, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc;
        rs_desc.Init_1_1(2, params, 0, nullptr,
                         D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> rs_blob, err_blob;
        throw_if_failed(
            D3DX12SerializeVersionedRootSignature(&rs_desc,
                D3D_ROOT_SIGNATURE_VERSION_1_1, &rs_blob, &err_blob),
            "PathTracer: serialize skinning root signature failed");
        throw_if_failed(
            device->CreateRootSignature(0,
                rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                IID_PPV_ARGS(&m_skinning_root_sig)),
            "PathTracer: create skinning root signature failed");
        m_skinning_root_sig->SetName(L"MARS::PathTracer::SkinningRootSig");
    }

    // ---- Compute PSO -------------------------------------------------------
    auto dxil = load_dxil_blob(L"skinning.dxil");

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_skinning_root_sig.Get();
    pso_desc.CS             = { dxil.data(), dxil.size() };

    throw_if_failed(
        device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_skinning_pso)),
        "PathTracer: create skinning compute PSO failed");
    m_skinning_pso->SetName(L"MARS::PathTracer::SkinningPSO");
}

// ---------------------------------------------------------------------------
// create_cloth_pipeline
// Same root signature layout as the skinning pipeline but with more root
// constants to cover all ClothConstants fields (28 DWORDs).
// ---------------------------------------------------------------------------
void PathTracer::create_cloth_pipeline(DeviceContext& ctx)
{
    auto* device = ctx.device();

    {
        // 28 root constants (all ClothConstants fields packed as DWORDs)
        CD3DX12_ROOT_PARAMETER1 params[2]{};
        params[0].InitAsConstants(28, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

        static constexpr UINT k_unbounded = UINT_MAX;
        static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS k_volatile =
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
            D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

        // cloth_sim.hlsl uses:
        //   t0, space1 — g_Buffers[]    (SRV: pos_prev, pos_curr, pos_pred_a/b reads)
        //   u0, space3 — g_RWBuffers[]  (UAV: pos_prev, pos_curr, pos_pred_a/b writes + output vertex buffer)
        CD3DX12_DESCRIPTOR_RANGE1 ranges[2]{};
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, k_unbounded, 0, 1, k_volatile, 0); // t0, space1 — SRV inputs
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, k_unbounded, 0, 3, k_volatile, 0); // u0, space3 — UAV outputs
        params[1].InitAsDescriptorTable(2, ranges, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc;
        rs_desc.Init_1_1(2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> rs_blob, err_blob;
        throw_if_failed(
            D3DX12SerializeVersionedRootSignature(&rs_desc,
                D3D_ROOT_SIGNATURE_VERSION_1_1, &rs_blob, &err_blob),
            "PathTracer: serialize cloth root signature failed");
        throw_if_failed(
            device->CreateRootSignature(0,
                rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                IID_PPV_ARGS(&m_cloth_root_sig)),
            "PathTracer: create cloth root signature failed");
        m_cloth_root_sig->SetName(L"MARS::PathTracer::ClothRootSig");
    }

    auto dxil = load_dxil_blob(L"cloth_sim.dxil");

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_cloth_root_sig.Get();
    pso_desc.CS             = { dxil.data(), dxil.size() };

    throw_if_failed(
        device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_cloth_pso)),
        "PathTracer: create cloth compute PSO failed");
    m_cloth_pso->SetName(L"MARS::PathTracer::ClothPSO");
}

// ---------------------------------------------------------------------------
// dispatch_cloth_sim
// Runs INTEGRATE pass then CONSTRAIN pass on the given ClothInstance.
// After this call, gpu.output_vertex_uav() contains the final deformed verts.
// ---------------------------------------------------------------------------
void PathTracer::dispatch_cloth_sim(ID3D12GraphicsCommandList6* cmd_list,
                                    ClothInstance&             ci,
                                    const Vec3&                wind,
                                    float                      delta_time)
{
    const ClothDesc& cd   = ci.cloth_desc;
    const uint32_t   n    = ci.vertex_count;
    const uint32_t   gw   = cd.grid_w;
    const uint32_t   gh   = cd.grid_h;

    static bool s_logged = false;
    if (!s_logged)
    {
        s_logged = true;
        MARS_LOG("[PathTracer] dispatch_cloth_sim '{}': n={} gw={} gh={}"
                     " pos_prev_srv={} pos_curr_srv={} pos_pred_a_uav={} out_vtx_uav={}"
                     " dt={} inv_mass={} rest_struct={} rest_shear={} rest_bend={}",
                     ci.name, n, gw, gh,
                     ci.gpu.pos_prev_srv(), ci.gpu.pos_curr_srv(),
                     ci.gpu.pos_pred_a_uav(), ci.gpu.output_vertex_uav(),
                     delta_time,
                     cd.mass > 0.0f ? 1.0f / cd.mass : 0.0f,
                     ci.rest_len_struct, ci.rest_len_shear, ci.rest_len_bend);
    }

    cmd_list->SetComputeRootSignature(m_cloth_root_sig.Get());
    cmd_list->SetPipelineState(m_cloth_pso.Get());
    cmd_list->SetComputeRootDescriptorTable(1, m_bindless_heap_gpu_start);

    // ClothConstants HLSL layout (28 DWORDs):
    //  [0]  grid_w       [1]  grid_h        [2]  vertex_count  [3]  delta_time
    //  [4..6] gravity    [7]  inv_mass
    //  [8..10] wind      [11] damping
    //  [12] structural_compliance  [13] shear_compliance  [14] bend_compliance
    //  [15] rest_len_struct  [16] rest_len_shear  [17] rest_len_bend
    //  [18] pos_prev_srv    [19] pos_curr_srv
    //  [20] pos_prev_uav    [21] pos_curr_uav
    //  [22] pos_pred_a_uav
    //  [23] output_vertex_uav
    //  [24] sim_pass
    //  [25] constrain_read_srv   [26] constrain_write_uav
    //  [27] pin_corners

    const float    inv_mass   = (cd.mass > 0.0f) ? (1.0f / cd.mass) : 0.0f;
    auto&          gpu        = ci.gpu;
    const uint32_t dispatch_x = (n + 63u) / 64u;

    D3D12_RESOURCE_BARRIER uav_barrier{};
    uav_barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barrier.UAV.pResource = nullptr;

    auto fill_common = [&](uint32_t c[28])
    {
        c[ 0] = gw;
        c[ 1] = gh;
        c[ 2] = n;
        c[ 3] = std::bit_cast<uint32_t>(delta_time);
        c[ 4] = std::bit_cast<uint32_t>(0.0f);
        c[ 5] = std::bit_cast<uint32_t>(-9.8f);
        c[ 6] = std::bit_cast<uint32_t>(0.0f);
        c[ 7] = std::bit_cast<uint32_t>(inv_mass);
        c[ 8] = std::bit_cast<uint32_t>(wind.x);
        c[ 9] = std::bit_cast<uint32_t>(wind.y);
        c[10] = std::bit_cast<uint32_t>(wind.z);
        c[11] = std::bit_cast<uint32_t>(cd.damping);
        c[12] = std::bit_cast<uint32_t>(cd.structural_compliance);
        c[13] = std::bit_cast<uint32_t>(cd.shear_compliance);
        c[14] = std::bit_cast<uint32_t>(cd.bend_compliance);
        c[15] = std::bit_cast<uint32_t>(ci.rest_len_struct);
        c[16] = std::bit_cast<uint32_t>(ci.rest_len_shear);
        c[17] = std::bit_cast<uint32_t>(ci.rest_len_bend);
        c[18] = gpu.pos_prev_srv();
        c[19] = gpu.pos_curr_srv();
        c[20] = gpu.pos_prev_uav();
        c[21] = gpu.pos_curr_uav();
        c[22] = gpu.pos_pred_a_uav();
        c[23] = gpu.output_vertex_uav();
    };

    // --- PASS 0: INTEGRATE ---------------------------------------------------
    {
        uint32_t c[28]{};
        fill_common(c);
        c[24] = 0u;   // sim_pass = INTEGRATE
        c[25] = 0u;   // constrain_read_srv  (unused)
        c[26] = 0u;   // constrain_write_uav (unused)
        c[27] = cd.pin_corners;
        cmd_list->SetComputeRoot32BitConstants(0, 28, c, 0);
        cmd_list->Dispatch(dispatch_x, 1, 1);
        cmd_list->ResourceBarrier(1, &uav_barrier);
    }

    // --- PASS 1: CONSTRAIN (xpbd_iterations times) ---------------------------
    // Ping-pongs between pred_a and pred_b.
    // First iteration reads pred_a (integrate output) and writes pred_b.
    bool read_pred_a = true;
    for (uint32_t iter = 0; iter < cd.xpbd_iterations; ++iter)
    {
        const uint32_t read_srv  = read_pred_a ? gpu.pos_pred_a_srv() : gpu.pos_pred_b_srv();
        const uint32_t write_uav = read_pred_a ? gpu.pos_pred_b_uav() : gpu.pos_pred_a_uav();

        uint32_t c[28]{};
        fill_common(c);
        c[24] = 1u;
        c[25] = read_srv;
        c[26] = write_uav;
        c[27] = cd.pin_corners;
        cmd_list->SetComputeRoot32BitConstants(0, 28, c, 0);
        cmd_list->Dispatch(dispatch_x, 1, 1);
        cmd_list->ResourceBarrier(1, &uav_barrier);

        read_pred_a = !read_pred_a;
    }

    // --- PASS 2: FINALIZE ----------------------------------------------------
    // Rotate state: pos_prev = old pos_curr, pos_curr = final prediction.
    // After the loop read_pred_a was flipped one extra time; the last
    // CONSTRAIN wrote to the buffer that is now "NOT read_pred_a".
    {
        const uint32_t final_srv = read_pred_a ? gpu.pos_pred_a_srv() : gpu.pos_pred_b_srv();

        uint32_t c[28]{};
        fill_common(c);
        c[24] = 2u;
        c[25] = final_srv;
        c[26] = 0u;
        c[27] = cd.pin_corners;
        cmd_list->SetComputeRoot32BitConstants(0, 28, c, 0);
        cmd_list->Dispatch(dispatch_x, 1, 1);
        cmd_list->ResourceBarrier(1, &uav_barrier);
    }
}

// ---------------------------------------------------------------------------
// create_vegetation_placement_pipeline
// Compute pipeline that reads a density map texture and writes vegetation
// instance data to a structured buffer with an atomic counter.
// ---------------------------------------------------------------------------
void PathTracer::create_vegetation_placement_pipeline(DeviceContext& ctx)
{
    auto* device = ctx.device();

    {
        // 16 root constants (PlacementConstants packed as DWORDs)
        CD3DX12_ROOT_PARAMETER1 params[2]{};
        params[0].InitAsConstants(16, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

        static constexpr UINT k_unbounded = UINT_MAX;
        static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS k_volatile =
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
            D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

        // vegetation_placement.hlsl uses:
        //   t0, space0 — g_Textures[]              (SRV: density map)
        //   u0, space0 — g_RWBuffersU32[]          (UAV: atomic counter)
        //   u0, space1 — g_RWInstanceBuffers[]     (UAV: instance buffer)
        CD3DX12_DESCRIPTOR_RANGE1 ranges[3]{};
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, k_unbounded, 0, 0, k_volatile, 0); // t0, space0
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, k_unbounded, 0, 0, k_volatile, 0); // u0, space0
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, k_unbounded, 0, 1, k_volatile, 0); // u0, space1
        params[1].InitAsDescriptorTable(3, ranges, D3D12_SHADER_VISIBILITY_ALL);

        // Static linear-clamp sampler for the density map at s0.
        CD3DX12_STATIC_SAMPLER_DESC static_sampler(
            0,                                          // shader register
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc;
        rs_desc.Init_1_1(2, params, 1, &static_sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> rs_blob, err_blob;
        throw_if_failed(
            D3DX12SerializeVersionedRootSignature(&rs_desc,
                D3D_ROOT_SIGNATURE_VERSION_1_1, &rs_blob, &err_blob),
            "PathTracer: serialize vegetation placement root signature failed");
        throw_if_failed(
            device->CreateRootSignature(0,
                rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                IID_PPV_ARGS(&m_vegetation_placement_root_sig)),
            "PathTracer: create vegetation placement root signature failed");
        m_vegetation_placement_root_sig->SetName(L"MARS::PathTracer::VegetationPlacementRootSig");
    }

    auto dxil = load_dxil_blob(L"vegetation_placement.dxil");

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_vegetation_placement_root_sig.Get();
    pso_desc.CS             = { dxil.data(), dxil.size() };

    throw_if_failed(
        device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_vegetation_placement_pso)),
        "PathTracer: create vegetation placement compute PSO failed");
    m_vegetation_placement_pso->SetName(L"MARS::PathTracer::VegetationPlacementPSO");

    MARS_LOG("[PathTracer] Vegetation placement compute pipeline created");
}

// ---------------------------------------------------------------------------
// dispatch_vegetation_placement
// Runs the GPU-driven vegetation placement compute shader. Reads the density
// map and writes instances to the ecosystem instance buffer + atomic counter.
//
// The caller is responsible for:
//   - Setting the bindless heap on the command list before calling
//   - Allocating ecosystem.instance_buffer_uav and instance counter buffer
//   - Setting up density_map_srv to point to a valid R8 texture
// ---------------------------------------------------------------------------
void PathTracer::dispatch_vegetation_placement(ID3D12GraphicsCommandList6* cmd_list,
                                                EcosystemDesc&              ecosystem)
{
    if (!ecosystem.enabled || ecosystem.species.empty())
        return;

    if (ecosystem.density_map_srv == UINT32_MAX || 
        ecosystem.instance_buffer_uav == UINT32_MAX)
    {
        MARS_LOG("[PathTracer] dispatch_vegetation_placement: ecosystem GPU resources not set up");
        return;
    }

    static bool s_logged = false;
    if (!s_logged)
    {
        s_logged = true;
        MARS_LOG("[PathTracer] dispatch_vegetation_placement: density_srv={} instance_uav={} max_instances={} species_count={}",
                 ecosystem.density_map_srv, ecosystem.instance_buffer_uav,
                 ecosystem.max_instances, ecosystem.species.size());
    }

    cmd_list->SetComputeRootSignature(m_vegetation_placement_root_sig.Get());
    cmd_list->SetPipelineState(m_vegetation_placement_pso.Get());
    cmd_list->SetComputeRootDescriptorTable(1, m_bindless_heap_gpu_start);

    // PlacementConstants HLSL layout (16 DWORDs):
    //   [0..2]  world_min (float3)
    //   [3]     placement_y (float)
    //   [4..6]  world_max (float3)
    //   [7]     max_instances (uint)
    //   [8]     density_multiplier (float)
    //   [9]     species_count (uint)
    //   [10]    density_map_srv (uint)
    //   [11]    instance_buffer_uav (uint)
    //   [12]    instance_counter_uav (uint)
    //   [13]    grid_resolution (uint)
    //   [14]    random_seed (uint)
    //   [15]    _pad0
    //
    // Size the candidate grid to roughly match max_instances. The placement
    // shader uses InterlockedAdd to claim instance slots, which is order-
    // dependent: if many more cells pass the spawn test than max_instances
    // permits, the "winning" cells vary between runs and the placed
    // instances appear in different positions each launch. Picking
    // grid_resolution ≈ sqrt(max_instances) ensures the number of candidate
    // spawn sites is on the order of max_instances, so even at density=1.0
    // the InterlockedAdd cap is rarely exceeded and placement is stable
    // (and, at max_instances == 1, fully deterministic).
    uint32_t grid_resolution = static_cast<uint32_t>(
        std::ceil(std::sqrt(static_cast<double>(std::max(1u, ecosystem.max_instances)))));
    grid_resolution = std::clamp(grid_resolution, 1u, 256u);

    union { float f; uint32_t u; } u;
    uint32_t c[16]{};
    u.f = ecosystem.world_min.x; c[0] = u.u;
    u.f = ecosystem.world_min.y; c[1] = u.u;
    u.f = ecosystem.world_min.z; c[2] = u.u;
    u.f = ecosystem.placement_y; c[3] = u.u;
    u.f = ecosystem.world_max.x; c[4] = u.u;
    u.f = ecosystem.world_max.y; c[5] = u.u;
    u.f = ecosystem.world_max.z; c[6] = u.u;
    c[7] = ecosystem.max_instances;
    u.f = ecosystem.density_multiplier; c[8] = u.u;
    c[9]  = static_cast<uint32_t>(ecosystem.species.size());
    c[10] = ecosystem.density_map_srv;
    c[11] = ecosystem.instance_buffer_uav;
    c[12] = ecosystem.instance_counter_uav;
    c[13] = grid_resolution;
    // Keep random_seed stable across the (one-shot) placement dispatch so
    // the same seed is used every launch — combined with a small candidate
    // grid this makes the resulting layout fully reproducible.
    c[14] = ecosystem.random_seed;
    c[15] = 0;                         // _pad0
    cmd_list->SetComputeRoot32BitConstants(0, 16, c, 0);

    const uint32_t dispatch_xy = (grid_resolution + 7) / 8; // 8x8 thread groups
    cmd_list->Dispatch(dispatch_xy, dispatch_xy, 1);
}

// ---------------------------------------------------------------------------
// create_vegetation_lod_pipeline
// Compute pipeline that reads vegetation instances + a per-species LOD-distance
// table and updates each instance's `current_lod` based on camera distance.
// ---------------------------------------------------------------------------
void PathTracer::create_vegetation_lod_pipeline(DeviceContext& ctx)
{
    auto* device = ctx.device();

    {
        // 12 root constants (LodSelectionConstants packed as DWORDs)
        CD3DX12_ROOT_PARAMETER1 params[2]{};
        params[0].InitAsConstants(12, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

        static constexpr UINT k_unbounded = UINT_MAX;
        static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS k_volatile =
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
            D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

        // vegetation_lod_selection.hlsl uses:
        //   u0, space1 — g_RWInstanceBuffers[]   (UAV: instance buffer, read+write)
        //   t0, space2 — g_SpeciesBuffers[]      (SRV: per-species LOD distances)
        CD3DX12_DESCRIPTOR_RANGE1 ranges[2]{};
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, k_unbounded, 0, 1, k_volatile, 0);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, k_unbounded, 0, 2, k_volatile, 0);
        params[1].InitAsDescriptorTable(2, ranges, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc;
        rs_desc.Init_1_1(2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> rs_blob, err_blob;
        throw_if_failed(
            D3DX12SerializeVersionedRootSignature(&rs_desc,
                D3D_ROOT_SIGNATURE_VERSION_1_1, &rs_blob, &err_blob),
            "PathTracer: serialize vegetation LOD root signature failed");
        throw_if_failed(
            device->CreateRootSignature(0,
                rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                IID_PPV_ARGS(&m_vegetation_lod_root_sig)),
            "PathTracer: create vegetation LOD root signature failed");
        m_vegetation_lod_root_sig->SetName(L"MARS::PathTracer::VegetationLodRootSig");
    }

    auto dxil = load_dxil_blob(L"vegetation_lod_selection.dxil");

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_vegetation_lod_root_sig.Get();
    pso_desc.CS             = { dxil.data(), dxil.size() };

    throw_if_failed(
        device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_vegetation_lod_pso)),
        "PathTracer: create vegetation LOD compute PSO failed");
    m_vegetation_lod_pso->SetName(L"MARS::PathTracer::VegetationLodPSO");

    MARS_LOG("[PathTracer] Vegetation LOD selection compute pipeline created");
}

// ---------------------------------------------------------------------------
// dispatch_vegetation_lod_selection
// Per-frame LOD update for all placed vegetation instances. Reads instance
// world positions, compares against the camera, and writes a new LOD tier
// + dither offset back into the instance buffer.
//
// `frame_index` is used to vary the per-frame dither jitter so transitions
// look animated rather than static.
// ---------------------------------------------------------------------------
void PathTracer::dispatch_vegetation_lod_selection(ID3D12GraphicsCommandList6* cmd_list,
                                                    EcosystemDesc&              ecosystem,
                                                    const Vec3&                 camera_position,
                                                    uint32_t                    frame_index)
{
    if (!ecosystem.enabled || ecosystem.species.empty() || ecosystem.instance_count == 0)
        return;

    if (ecosystem.instance_buffer_uav == UINT32_MAX ||
        ecosystem.species_buffer_srv  == UINT32_MAX)
    {
        MARS_LOG("[PathTracer] dispatch_vegetation_lod_selection: ecosystem GPU resources not set up");
        return;
    }

    cmd_list->SetComputeRootSignature(m_vegetation_lod_root_sig.Get());
    cmd_list->SetPipelineState(m_vegetation_lod_pso.Get());
    cmd_list->SetComputeRootDescriptorTable(1, m_bindless_heap_gpu_start);

    // LodSelectionConstants HLSL layout (12 DWORDs):
    //   [0..2]  camera_position (float3)
    //   [3]     instance_count (uint)
    //   [4]     species_count (uint)
    //   [5]     instance_buffer_uav (uint)
    //   [6]     species_buffer_srv (uint)
    //   [7]     frame_jitter_seed (uint)
    //   [8]     dither_band_meters (float)
    //   [9..11] _pad
    union { float f; uint32_t u; } u;
    uint32_t c[12]{};
    u.f = camera_position.x; c[0] = u.u;
    u.f = camera_position.y; c[1] = u.u;
    u.f = camera_position.z; c[2] = u.u;
    c[3]  = ecosystem.instance_count;
    c[4]  = static_cast<uint32_t>(ecosystem.species.size());
    c[5]  = ecosystem.instance_buffer_uav;
    c[6]  = ecosystem.species_buffer_srv;
    c[7]  = frame_index;
    u.f = ecosystem.lod_dither_band_meters; c[8] = u.u;
    cmd_list->SetComputeRoot32BitConstants(0, 12, c, 0);

    const uint32_t group_count = (ecosystem.instance_count + 63u) / 64u;
    cmd_list->Dispatch(group_count, 1, 1);

    // UAV barrier so subsequent passes see the updated LOD values.
    auto uav_barrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
    cmd_list->ResourceBarrier(1, &uav_barrier);
}

// ---------------------------------------------------------------------------
// create_vegetation_wind_pipeline
// Compute pipeline that deforms a static rest-pose vegetation mesh into a
// wind-driven output vertex buffer suitable for refittable BLAS consumption.
// Uses the same bindless byte-address-buffer layout as the skinning pass so
// the existing refit_blas() path can be reused on the output buffer.
// ---------------------------------------------------------------------------
void PathTracer::create_vegetation_wind_pipeline(DeviceContext& ctx)
{
    auto* device = ctx.device();

    {
        // 16 root constants (WindConstants packed as DWORDs)
        CD3DX12_ROOT_PARAMETER1 params[2]{};
        params[0].InitAsConstants(16, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

        static constexpr UINT k_unbounded = UINT_MAX;
        static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS k_volatile =
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
            D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

        // vegetation_wind.hlsl uses (matches skinning layout):
        //   t0, space1 — g_Buffers[]        (SRV: source ByteAddressBuffer)
        //   u0, space3 — g_OutputBuffers[]  (UAV: deformed RWByteAddressBuffer)
        CD3DX12_DESCRIPTOR_RANGE1 ranges[2]{};
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, k_unbounded, 0, 1, k_volatile, 0);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, k_unbounded, 0, 3, k_volatile, 0);
        params[1].InitAsDescriptorTable(2, ranges, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc;
        rs_desc.Init_1_1(2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> rs_blob, err_blob;
        throw_if_failed(
            D3DX12SerializeVersionedRootSignature(&rs_desc,
                D3D_ROOT_SIGNATURE_VERSION_1_1, &rs_blob, &err_blob),
            "PathTracer: serialize vegetation wind root signature failed");
        throw_if_failed(
            device->CreateRootSignature(0,
                rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                IID_PPV_ARGS(&m_vegetation_wind_root_sig)),
            "PathTracer: create vegetation wind root signature failed");
        m_vegetation_wind_root_sig->SetName(L"MARS::PathTracer::VegetationWindRootSig");
    }

    auto dxil = load_dxil_blob(L"vegetation_wind.dxil");

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_vegetation_wind_root_sig.Get();
    pso_desc.CS             = { dxil.data(), dxil.size() };

    throw_if_failed(
        device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_vegetation_wind_pso)),
        "PathTracer: create vegetation wind compute PSO failed");
    m_vegetation_wind_pso->SetName(L"MARS::PathTracer::VegetationWindPSO");

    MARS_LOG("[PathTracer] Vegetation wind compute pipeline created");
}

// ---------------------------------------------------------------------------
// dispatch_vegetation_wind
// Deform a single vegetation mesh into the supplied output vertex buffer.
// Caller is responsible for issuing a UAV barrier on the output buffer and
// invoking refit_blas() on the species' refittable BLAS afterwards.
// ---------------------------------------------------------------------------
void PathTracer::dispatch_vegetation_wind(ID3D12GraphicsCommandList6* cmd_list,
                                          uint32_t       vertex_count,
                                          uint32_t       source_vertex_srv,
                                          uint32_t       output_vertex_uav,
                                          uint32_t       prev_pos_uav,
                                          float          mesh_min_y,
                                          float          mesh_height,
                                          const Vec3&    wind_direction,
                                          float          wind_strength,
                                          float          time_seconds,
                                          float          wind_phase_offset,
                                          float          primary_bend,
                                          float          secondary_sway,
                                          float          leaf_flutter,
                                          float          trunk_envelope)
{
    if (vertex_count == 0 ||
        source_vertex_srv == UINT32_MAX ||
        output_vertex_uav == UINT32_MAX)
        return;

    cmd_list->SetComputeRootSignature(m_vegetation_wind_root_sig.Get());
    cmd_list->SetPipelineState(m_vegetation_wind_pso.Get());
    cmd_list->SetComputeRootDescriptorTable(1, m_bindless_heap_gpu_start);

    // WindConstants HLSL layout (16 DWORDs):
    //   [0]     vertex_count
    //   [1]     source_vertex_buffer_srv
    //   [2]     output_vertex_buffer_uav
    //   [3]     mesh_min_y
    //   [4..6]  wind_direction
    //   [7]     wind_strength
    //   [8]     time_seconds
    //   [9]     wind_phase_offset
    //   [10]    primary_bend
    //   [11]    secondary_sway
    //   [12]    leaf_flutter
    //   [13]    mesh_height
    //   [14]    prev_pos_buffer_uav
    //   [15]    trunk_envelope  (spring-mass oscillator output, replaces raw wind_t in Layer 1)
    union { float f; uint32_t u; } u;
    uint32_t c[16]{};
    c[0] = vertex_count;
    c[1] = source_vertex_srv;
    c[2] = output_vertex_uav;
    u.f = mesh_min_y;        c[3]  = u.u;
    u.f = wind_direction.x;  c[4]  = u.u;
    u.f = wind_direction.y;  c[5]  = u.u;
    u.f = wind_direction.z;  c[6]  = u.u;
    u.f = wind_strength;     c[7]  = u.u;
    u.f = time_seconds;      c[8]  = u.u;
    u.f = wind_phase_offset; c[9]  = u.u;
    u.f = primary_bend;      c[10] = u.u;
    u.f = secondary_sway;    c[11] = u.u;
    u.f = leaf_flutter;      c[12] = u.u;
    u.f = mesh_height;       c[13] = u.u;
    c[14] = prev_pos_uav;
    u.f = trunk_envelope;    c[15] = u.u;
    cmd_list->SetComputeRoot32BitConstants(0, 16, c, 0);

    const uint32_t group_count = (vertex_count + 63u) / 64u;
    cmd_list->Dispatch(group_count, 1, 1);
}

// ---------------------------------------------------------------------------
// create_rtpso
// Loads path_trace.dxil, builds the DXR pipeline state object.
// ---------------------------------------------------------------------------
void PathTracer::create_rtpso(DeviceContext& ctx)
{
    auto* device = ctx.device();

    // ---- 1. Load DXIL blob -----------------------------------------------
    auto dxil = load_dxil_blob(L"path_trace.dxil");

    // ---- 2. Global root signature ----------------------------------------
    // Layout:
    //   b0  space0 : FrameConstants CBV (root CBV, inline)
    //   Descriptor table (bindless heap) bound at the start; shaders index via
    //   the slot indices stored in FrameConstants.
    {
        CD3DX12_ROOT_PARAMETER1 params[2]{};

        // Param 0: per-frame CBV (FrameConstants) bound as root CBV
        params[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
                                           D3D12_SHADER_VISIBILITY_ALL);

        // Param 1: descriptor table covering the whole bindless heap (SRV + UAV).
        // Each HLSL register space needs its own range; a single range cannot
        // span multiple spaces.
        //
        // SRV spaces used by the path-tracing shaders:
        //   space0 — g_Textures[]          (t0, space0)
        //   space1 — g_Buffers[]           (t0, space1)
        //   space2 — g_MaterialBuffer[]    (t0, space2)
        //   space3 — g_InstanceBuffer[]    (t0, space3)
        //   space4 — g_TLAS[]             (t0, space4)
        // UAV spaces:
        //   space0 — g_RWBuffers[]         (u0, space0)
        //   space1 — g_OutputUAV[]         (u0, space1)
        //   space2 — g_MotionVectorUAV[]   (u0, space2)
        //   space3 — g_LinearDepthUAV[]    (u0, space3)
        //   space5 — g_AlbedoUAV[]         (u0, space5)  [M7]
        //   space6 — g_SpecularAlbedoUAV[] (u0, space6)  [M7]
        //   space7 — g_NormalsUAV[]        (u0, space7)  [M7]
        //   space8 — g_RoughnessUAV[]      (u0, space8)  [M7]
        static constexpr UINT k_unbounded = UINT_MAX;
        static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS k_volatile_flags =
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
            D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

        // All register-space arrays in HLSL are backed by the same physical bindless
        // heap starting at descriptor index 0.  Every range must therefore specify
        // offsetInDescriptorsFromTableStart = 0 explicitly; using the default
        // D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND after an unbounded (UINT_MAX) range
        // causes an arithmetic overflow and E_INVALIDARG during serialization.
        CD3DX12_DESCRIPTOR_RANGE1 ranges[14]{};
        // SRV space0 – g_Textures[]
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, k_unbounded, 0, 0, k_volatile_flags, 0);
        // SRV space1 – g_Buffers[]
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, k_unbounded, 0, 1, k_volatile_flags, 0);
        // SRV space2 – g_MaterialBuffer[]
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, k_unbounded, 0, 2, k_volatile_flags, 0);
        // SRV space3 – g_InstanceBuffer[]
        ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, k_unbounded, 0, 3, k_volatile_flags, 0);
        // SRV space4 – g_TLAS[]
        ranges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, k_unbounded, 0, 4, k_volatile_flags, 0);
        // UAV space0 – g_RWBuffers[]
        ranges[5].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, k_unbounded, 0, 0, k_volatile_flags, 0);
        // UAV space1 – g_OutputUAV[]
        ranges[6].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, k_unbounded, 0, 1, k_volatile_flags, 0);
        // UAV space2 – g_MotionVectorUAV[]
        ranges[7].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, k_unbounded, 0, 2, k_volatile_flags, 0);
        // UAV space3 – g_LinearDepthUAV[]
        ranges[8].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, k_unbounded, 0, 3, k_volatile_flags, 0);
        // UAV space5 – g_AlbedoUAV[]         [M7]
        ranges[9].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, k_unbounded, 0, 5, k_volatile_flags, 0);
        // UAV space6 – g_SpecularAlbedoUAV[] [M7]
        ranges[10].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, k_unbounded, 0, 6, k_volatile_flags, 0);
        // UAV space7 – g_NormalsUAV[]        [M7]
        ranges[11].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, k_unbounded, 0, 7, k_volatile_flags, 0);
        // UAV space8 – g_RoughnessUAV[]      [M7]
        ranges[12].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, k_unbounded, 0, 8, k_volatile_flags, 0);
        // UAV space9 – g_GIReservoirs[]       [M8]
        ranges[13].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, k_unbounded, 0, 9, k_volatile_flags, 0);

        params[1].InitAsDescriptorTable(14, ranges, D3D12_SHADER_VISIBILITY_ALL);

        // Static sampler s0: linear wrap
        CD3DX12_STATIC_SAMPLER_DESC sampler(0,
            D3D12_FILTER_ANISOTROPIC,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            0.0f, 16);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc;
        rs_desc.Init_1_1(2, params, 1, &sampler,
                         D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> rs_blob, err_blob;
        throw_if_failed(
            D3DX12SerializeVersionedRootSignature(&rs_desc,
                D3D_ROOT_SIGNATURE_VERSION_1_1, &rs_blob, &err_blob),
            "PathTracer: serialize global root signature failed");

        throw_if_failed(
            device->CreateRootSignature(0,
                rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                IID_PPV_ARGS(&m_global_root_sig)),
            "PathTracer: create global root signature failed");

        m_global_root_sig->SetName(L"MARS::PathTracer::GlobalRootSig");
    }

    // Empty local root signature (no per-shader-record data in M4)
    {
        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc;
        rs_desc.Init_1_1(0, nullptr, 0, nullptr,
                         D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE);

        ComPtr<ID3DBlob> rs_blob, err_blob;
        throw_if_failed(
            D3DX12SerializeVersionedRootSignature(&rs_desc,
                D3D_ROOT_SIGNATURE_VERSION_1_1, &rs_blob, &err_blob),
            "PathTracer: serialize local root signature failed");

        throw_if_failed(
            device->CreateRootSignature(0,
                rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                IID_PPV_ARGS(&m_local_root_sig)),
            "PathTracer: create local root signature failed");

        m_local_root_sig->SetName(L"MARS::PathTracer::LocalRootSig");
    }

    // ---- 3. Build CD3DX12_STATE_OBJECT_DESC ---------------------------------
    CD3DX12_STATE_OBJECT_DESC so_desc(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);

    // DXIL library subobject
    auto* lib = so_desc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    D3D12_SHADER_BYTECODE dxil_bc = { dxil.data(), dxil.size() };
    lib->SetDXILLibrary(&dxil_bc);
    // Export all entry points (RayGen, ClosestHit, Miss, ShadowMiss)
    lib->DefineExport(L"RayGen");
    lib->DefineExport(L"ClosestHit");
    lib->DefineExport(L"AnyHit_Primary");
    lib->DefineExport(L"Miss");
    lib->DefineExport(L"ShadowMiss");
    lib->DefineExport(L"Intersection_Impostor");
    lib->DefineExport(L"ClosestHit_Impostor");

    // Hit group subobject (primary)
    auto* hg = so_desc.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hg->SetClosestHitShaderImport(L"ClosestHit");
    hg->SetAnyHitShaderImport(L"AnyHit_Primary");
    hg->SetHitGroupExport(L"HitGroup_Primary");
    hg->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

    // Hit group subobject (octahedral impostor — procedural AABB BLAS)
    auto* hg_imp = so_desc.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hg_imp->SetIntersectionShaderImport(L"Intersection_Impostor");
    hg_imp->SetClosestHitShaderImport(L"ClosestHit_Impostor");
    hg_imp->SetHitGroupExport(L"HitGroup_Impostor");
    hg_imp->SetHitGroupType(D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE);

    // Shader config: payload = float3 radiance + float hit_t + uint missed + uint depth + float3 throughput + float3 sec_normal
    // Total: 12 + 4 + 4 + 4 + 12 + 12 = 48 bytes (C++ layout).
    // The compiled HLSL Miss shader reports 56 bytes — the HLSL payload struct
    // contains an extra 8 bytes (e.g. padding or an additional field).  This
    // value must be >= the largest payload size reported by any shader in the
    // library; use 56 to match what the driver validates against.
    auto* shader_cfg = so_desc.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    shader_cfg->Config(
        56,                // payload size: 56 bytes (matches compiled HLSL Miss shader)
        sizeof(float) * 4);                   // attribute size: max(barycentrics=8, ImpostorAttr=16)

    // Pipeline config: max recursion depth 3 (primary + up to 2 GI bounces; shadow is a separate non-recursive dispatch)
    auto* pipe_cfg = so_desc.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    pipe_cfg->Config(3);

    // Global root signature
    auto* grs = so_desc.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    grs->SetRootSignature(m_global_root_sig.Get());

    // Local root signature (empty) associated with all exports
    auto* lrs = so_desc.CreateSubobject<CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();
    lrs->SetRootSignature(m_local_root_sig.Get());
    auto* lrs_assoc = so_desc.CreateSubobject<CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
    lrs_assoc->SetSubobjectToAssociate(*lrs);
    lrs_assoc->AddExport(L"RayGen");
    lrs_assoc->AddExport(L"ClosestHit");
    lrs_assoc->AddExport(L"AnyHit_Primary");
    lrs_assoc->AddExport(L"Miss");
    lrs_assoc->AddExport(L"ShadowMiss");
    lrs_assoc->AddExport(L"HitGroup_Primary");
    lrs_assoc->AddExport(L"Intersection_Impostor");
    lrs_assoc->AddExport(L"ClosestHit_Impostor");
    lrs_assoc->AddExport(L"HitGroup_Impostor");

    // ---- 4. Create RTPSO ----------------------------------------------------
    throw_if_failed(
        device->CreateStateObject(so_desc, IID_PPV_ARGS(&m_rtpso)),
        "PathTracer: CreateStateObject (RTPSO) failed");
    m_rtpso->SetName(L"MARS::PathTracer::RTPSO");

    throw_if_failed(
        m_rtpso->QueryInterface(IID_PPV_ARGS(&m_rtpso_props)),
        "PathTracer: QueryInterface ID3D12StateObjectProperties failed");
}

// ---------------------------------------------------------------------------
// create_shader_tables
// ---------------------------------------------------------------------------
void PathTracer::create_shader_tables(DeviceContext& ctx)
{
    // Ray-gen table: 1 record (aligned to D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT)
    uint64_t raygen_size = align_up(k_raygen_record_stride,
                                    D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

    // Miss table: 2 records (Miss + ShadowMiss), stride = k_miss_record_stride
    uint32_t miss_count  = 2;
    uint64_t miss_size   = align_up(k_miss_record_stride * miss_count,
                                    D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

    // Hit group table: 2 records (HitGroup_Primary, HitGroup_Impostor)
    uint32_t hitgroup_count = 2;
    uint64_t hitgroup_size = align_up(k_hitgroup_record_stride * hitgroup_count,
                                      D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

    auto fill_table = [&](uint64_t size, const wchar_t* name,
                          D3D12MA::Allocation** out_alloc, ID3D12Resource** out_res) -> void*
    {
        void* mapped = nullptr;
        create_upload_buffer(ctx, size, name, out_alloc, out_res, &mapped);
        ZeroMemory(mapped, size);
        return mapped;
    };

    // Ray-gen
    void* raygen_ptr = fill_table(raygen_size, L"MARS::ShaderTable::RayGen",
                                  &m_raygen_table_alloc, &m_raygen_table);
    memcpy(raygen_ptr,
           m_rtpso_props->GetShaderIdentifier(L"RayGen"),
           k_shader_id_size);

    // Miss (2 records)
    void* miss_ptr = fill_table(miss_size, L"MARS::ShaderTable::Miss",
                                &m_miss_table_alloc, &m_miss_table);
    memcpy(static_cast<uint8_t*>(miss_ptr),
           m_rtpso_props->GetShaderIdentifier(L"Miss"),
           k_shader_id_size);
    memcpy(static_cast<uint8_t*>(miss_ptr) + k_miss_record_stride,
           m_rtpso_props->GetShaderIdentifier(L"ShadowMiss"),
           k_shader_id_size);

    // Hit group
    void* hg_ptr = fill_table(hitgroup_size, L"MARS::ShaderTable::HitGroup",
                               &m_hitgroup_table_alloc, &m_hitgroup_table);
    memcpy(hg_ptr,
           m_rtpso_props->GetShaderIdentifier(L"HitGroup_Primary"),
           k_shader_id_size);
    memcpy(static_cast<uint8_t*>(hg_ptr) + k_hitgroup_record_stride,
           m_rtpso_props->GetShaderIdentifier(L"HitGroup_Impostor"),
           k_shader_id_size);
}

// ---------------------------------------------------------------------------
// create_output_textures
// Allocates one RGBA16F DEFAULT-heap UAV texture per output.
// Also primes motion vector and depth texture placeholders.
// ---------------------------------------------------------------------------
void PathTracer::create_output_textures(DeviceContext& ctx)
{
    m_outputs.resize(m_output_count);
    m_mv_outputs.resize(m_output_count);
    m_depth_outputs.resize(m_output_count);
    m_denoised_outputs.resize(m_output_count);
    m_albedo_outputs.resize(m_output_count);
    m_specular_albedo_outputs.resize(m_output_count);
    m_normals_aov_outputs.resize(m_output_count);
    m_roughness_aov_outputs.resize(m_output_count);
    m_gi_reservoir_buffers.resize(m_output_count);

    for (uint32_t i = 0; i < m_output_count; ++i)
    {
        // Placeholder — actual textures are created/re-created in resize_output().
        m_outputs[i].width                   = 0;
        m_outputs[i].height                  = 0;
        m_mv_outputs[i].width                = 0;
        m_mv_outputs[i].height               = 0;
        m_depth_outputs[i].width             = 0;
        m_depth_outputs[i].height            = 0;
        m_denoised_outputs[i].width          = 0;
        m_denoised_outputs[i].height         = 0;
        m_albedo_outputs[i].width            = 0;
        m_albedo_outputs[i].height           = 0;
        m_specular_albedo_outputs[i].width   = 0;
        m_specular_albedo_outputs[i].height  = 0;
        m_normals_aov_outputs[i].width       = 0;
        m_normals_aov_outputs[i].height      = 0;
        m_roughness_aov_outputs[i].width     = 0;
        m_roughness_aov_outputs[i].height    = 0;
    }
    (void)ctx;
}

void PathTracer::release_output_textures()
{
    auto release_vec = [](std::vector<OutputTexture>& v)
    {
        for (auto& out : v)
        {
            if (out.alloc) { out.alloc->Release(); out.alloc = nullptr; out.resource = nullptr; }
            out.uav_slot = UINT32_MAX;
            out.srv_slot = UINT32_MAX;
        }
    };
    release_vec(m_outputs);
    release_vec(m_mv_outputs);
    release_vec(m_depth_outputs);
    release_vec(m_denoised_outputs);
    release_vec(m_albedo_outputs);
    release_vec(m_specular_albedo_outputs);
    release_vec(m_normals_aov_outputs);
    release_vec(m_roughness_aov_outputs);

    // Release GI reservoir structured buffers (M8)
    for (auto& gi : m_gi_reservoir_buffers)
    {
        if (gi.alloc) { gi.alloc->Release(); gi.alloc = nullptr; gi.resource = nullptr; }
        gi.uav_slot = UINT32_MAX;
    }
    m_gi_reservoir_buffers.clear();
}

// ---------------------------------------------------------------------------
// create_cb_ring
// One FrameConstants upload buffer per output × k_frame_count.
// ---------------------------------------------------------------------------
void PathTracer::create_cb_ring(DeviceContext& ctx)
{
    uint32_t total = m_output_count * k_frame_count;
    m_frame_cbs.resize(total);

    for (uint32_t i = 0; i < total; ++i)
    {
        FrameCBSlot& slot = m_frame_cbs[i];
        create_upload_buffer(ctx, sizeof(FrameConstants),
                             L"MARS::PathTracer::FrameCB",
                             &slot.alloc, &slot.resource, &slot.mapped_ptr);
    }
}

// ---------------------------------------------------------------------------
// resize_output
// (Re)creates the UAV texture for the given output at the new dimensions.
// new_width/new_height   — render resolution (path-tracer fires rays at this size).
// display_width/height   — display resolution for the DLSS denoised output texture.
//                          Pass 0 to use the same dimensions as render (no upscaling).
// ---------------------------------------------------------------------------
void PathTracer::resize_output(DeviceContext& ctx,
                                uint32_t output_index,
                                uint32_t new_width, uint32_t new_height,
                                uint32_t display_width, uint32_t display_height)
{
    // Default denoised-output size to render size when not upscaling.
    if (display_width  == 0) display_width  = new_width;
    if (display_height == 0) display_height = new_height;

    assert(output_index < m_output_count);
    OutputTexture& out = m_outputs[output_index];

    // Skip reallocation only when both render AND display dims are unchanged.
    {
        OutputTexture& dn = m_denoised_outputs[output_index];
        if (out.width == new_width && out.height == new_height &&
            dn.width  == display_width && dn.height == display_height) return;
    }

    // Release old render-res texture
    if (out.alloc) { out.alloc->Release(); out.alloc = nullptr; out.resource = nullptr; }

    out.width  = new_width;
    out.height = new_height;

    // Create RGBA16F texture in DEFAULT heap with UAV flag
    D3D12MA::ALLOCATION_DESC alloc_desc{};
    alloc_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width              = new_width;
    tex_desc.Height             = new_height;
    tex_desc.DepthOrArraySize   = 1;
    tex_desc.MipLevels          = 1;
    tex_desc.Format             = DXGI_FORMAT_R16G16B16A16_FLOAT;
    tex_desc.SampleDesc.Count   = 1;
    tex_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex_desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    throw_if_failed(
        m_allocator->CreateResource(
            &alloc_desc, &tex_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            &out.alloc,
            IID_PPV_ARGS(&out.resource)),
        "PathTracer: create output UAV texture failed");

    out.resource->SetName(
        std::wstring(L"MARS::PathTracer::OutputUAV[" + std::to_wstring(output_index) + L"]").c_str());

    // Allocate bindless UAV slot
    auto* device = ctx.device();
    out.uav_slot = ctx.allocate_bindless_slot();

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
    cpu_handle.ptr += static_cast<SIZE_T>(out.uav_slot) * ctx.bindless_descriptor_size();
    device->CreateUnorderedAccessView(out.resource, nullptr, &uav_desc, cpu_handle);

    // Allocate bindless SRV slot (used by the tone-map blit pixel shader)
    out.srv_slot = ctx.allocate_bindless_slot();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format                        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv_desc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels           = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE srv_handle = ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
    srv_handle.ptr += static_cast<SIZE_T>(out.srv_slot) * ctx.bindless_descriptor_size();
    device->CreateShaderResourceView(out.resource, &srv_desc, srv_handle);

    // ---- Motion vector texture (R16G16B16A16_FLOAT) -------------------------
    {
        OutputTexture& mv = m_mv_outputs[output_index];
        if (mv.alloc) { mv.alloc->Release(); mv.alloc = nullptr; mv.resource = nullptr; }
        mv.width  = new_width;
        mv.height = new_height;

        D3D12_RESOURCE_DESC mv_desc = tex_desc;
        mv_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

        throw_if_failed(
            m_allocator->CreateResource(
                &alloc_desc, &mv_desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr,
                &mv.alloc,
                IID_PPV_ARGS(&mv.resource)),
            "PathTracer: create motion vector UAV texture failed");
        mv.resource->SetName(
            std::wstring(L"MARS::PathTracer::MotionVectorUAV[" +
                         std::to_wstring(output_index) + L"]").c_str());

        mv.uav_slot = ctx.allocate_bindless_slot();
        D3D12_UNORDERED_ACCESS_VIEW_DESC mv_uav_desc{};
        mv_uav_desc.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        mv_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE mv_cpu = ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
        mv_cpu.ptr += static_cast<SIZE_T>(mv.uav_slot) * ctx.bindless_descriptor_size();
        device->CreateUnorderedAccessView(mv.resource, nullptr, &mv_uav_desc, mv_cpu);
    }

    // ---- Linear depth texture (R32_FLOAT) -----------------------------------
    {
        OutputTexture& d = m_depth_outputs[output_index];
        if (d.alloc) { d.alloc->Release(); d.alloc = nullptr; d.resource = nullptr; }
        d.width  = new_width;
        d.height = new_height;

        D3D12_RESOURCE_DESC d_desc = tex_desc;
        d_desc.Format = DXGI_FORMAT_R32_FLOAT;

        throw_if_failed(
            m_allocator->CreateResource(
                &alloc_desc, &d_desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr,
                &d.alloc,
                IID_PPV_ARGS(&d.resource)),
            "PathTracer: create depth UAV texture failed");
        d.resource->SetName(
            std::wstring(L"MARS::PathTracer::LinearDepthUAV[" +
                         std::to_wstring(output_index) + L"]").c_str());

        d.uav_slot = ctx.allocate_bindless_slot();
        D3D12_UNORDERED_ACCESS_VIEW_DESC d_uav_desc{};
        d_uav_desc.Format        = DXGI_FORMAT_R32_FLOAT;
        d_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE d_cpu = ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
        d_cpu.ptr += static_cast<SIZE_T>(d.uav_slot) * ctx.bindless_descriptor_size();
        device->CreateUnorderedAccessView(d.resource, nullptr, &d_uav_desc, d_cpu);
    }

    // ---- Denoised output texture (RGBA16F) — separate from m_outputs -------
    // DLSS-RR reads from m_outputs (noisy, render-res) and writes to m_denoised_outputs
    // at DISPLAY resolution (after upscaling).  Must be allocated at display_width x display_height.
    {
        OutputTexture& dn = m_denoised_outputs[output_index];
        if (dn.alloc) { dn.alloc->Release(); dn.alloc = nullptr; dn.resource = nullptr; }
        dn.width  = display_width;
        dn.height = display_height;

        D3D12_RESOURCE_DESC dn_desc = tex_desc; // RGBA16F, UAV flag
        dn_desc.Width  = display_width;
        dn_desc.Height = display_height;

        // Create in COMMON state so Streamline can issue valid legacy ResourceBarrier
        // transitions from COMMON on every frame. After each slEvaluateFeature call the
        // renderer issues an Enhanced Barrier to bring the resource back from
        // D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS to D3D12_BARRIER_LAYOUT_COMMON, which
        // re-enables legacy barrier compatibility for the next frame.
        throw_if_failed(
            m_allocator->CreateResource(
                &alloc_desc, &dn_desc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                &dn.alloc,
                IID_PPV_ARGS(&dn.resource)),
            "PathTracer: create denoised output UAV texture failed");
        dn.resource->SetName(
            std::wstring(L"MARS::PathTracer::DenoisedOutputUAV[" +
                         std::to_wstring(output_index) + L"]").c_str());

        dn.uav_slot = ctx.allocate_bindless_slot();
        D3D12_UNORDERED_ACCESS_VIEW_DESC dn_uav_desc{};
        dn_uav_desc.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dn_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE dn_cpu = ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
        dn_cpu.ptr += static_cast<SIZE_T>(dn.uav_slot) * ctx.bindless_descriptor_size();
        device->CreateUnorderedAccessView(dn.resource, nullptr, &dn_uav_desc, dn_cpu);

        // SRV so copy_to_back_buffer can read the denoised result
        dn.srv_slot = ctx.allocate_bindless_slot();
        D3D12_SHADER_RESOURCE_VIEW_DESC dn_srv_desc{};
        dn_srv_desc.Format                        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dn_srv_desc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
        dn_srv_desc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dn_srv_desc.Texture2D.MipLevels           = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE dn_srv_handle = ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
        dn_srv_handle.ptr += static_cast<SIZE_T>(dn.srv_slot) * ctx.bindless_descriptor_size();
        device->CreateShaderResourceView(dn.resource, &dn_srv_desc, dn_srv_handle);
    }

    // ---- AOV textures for DLSS-RR tagging -----------------------------------
    // Albedo, SpecularAlbedo, Normals, Roughness — all RGBA16F UAVs at render res,
    // created in UAV state. Written black until M7/M8 G-buffer shaders are added.
    // Renderer transitions these UAV → COMMON before tag_resources and back to UAV
    // after slEvaluateFeature, mirroring the noisy_color / mvec / depth pattern.
    struct AovEntry { std::vector<OutputTexture>& vec; const wchar_t* name; };
    AovEntry aov_entries[] =
    {
        { m_albedo_outputs,           L"MARS::PathTracer::AlbedoAOV["          },
        { m_specular_albedo_outputs,  L"MARS::PathTracer::SpecularAlbedoAOV["  },
        { m_normals_aov_outputs,      L"MARS::PathTracer::NormalsAOV["         },
        { m_roughness_aov_outputs,    L"MARS::PathTracer::RoughnessAOV["       },
    };
    for (auto& entry : aov_entries)
    {
        OutputTexture& aov = entry.vec[output_index];
        if (aov.alloc) { aov.alloc->Release(); aov.alloc = nullptr; aov.resource = nullptr; }
        aov.width  = new_width;
        aov.height = new_height;

        D3D12_RESOURCE_DESC aov_desc = tex_desc; // inherits RGBA16F and UAV flag
        throw_if_failed(
            m_allocator->CreateResource(
                &alloc_desc, &aov_desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr,
                &aov.alloc,
                IID_PPV_ARGS(&aov.resource)),
            "PathTracer: create AOV UAV texture failed");
        aov.resource->SetName(
            std::wstring(entry.name + std::to_wstring(output_index) + L"]").c_str());

        aov.uav_slot = ctx.allocate_bindless_slot();
        D3D12_UNORDERED_ACCESS_VIEW_DESC aov_uav_desc{};
        aov_uav_desc.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        aov_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE aov_cpu = ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
        aov_cpu.ptr += static_cast<SIZE_T>(aov.uav_slot) * ctx.bindless_descriptor_size();
        device->CreateUnorderedAccessView(aov.resource, nullptr, &aov_uav_desc, aov_cpu);
    }

    // ---- GI reservoir structured buffer (M8) --------------------------------
    // Two layers (ping-pong temporal reuse): total = width * height * 2 elements.
    // Each element is a GIReservoir (must match the HLSL struct; 48 bytes).
    {
        static constexpr uint32_t k_gi_reservoir_stride = 64u; // sizeof GIReservoir in HLSL
        GIBuffer& gi = m_gi_reservoir_buffers[output_index];
        if (gi.alloc) { gi.alloc->Release(); gi.alloc = nullptr; gi.resource = nullptr; }
        gi.width  = new_width;
        gi.height = new_height;

        uint64_t element_count = static_cast<uint64_t>(new_width) * new_height * 2u;
        uint64_t byte_size     = element_count * k_gi_reservoir_stride;

        D3D12MA::ALLOCATION_DESC gi_alloc_desc{};
        gi_alloc_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC gi_buf_desc{};
        gi_buf_desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        gi_buf_desc.Width            = byte_size;
        gi_buf_desc.Height           = 1;
        gi_buf_desc.DepthOrArraySize = 1;
        gi_buf_desc.MipLevels        = 1;
        gi_buf_desc.SampleDesc.Count = 1;
        gi_buf_desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        gi_buf_desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        throw_if_failed(
            m_allocator->CreateResource(
                &gi_alloc_desc, &gi_buf_desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr,
                &gi.alloc,
                IID_PPV_ARGS(&gi.resource)),
            "PathTracer: create GI reservoir buffer failed");
        gi.resource->SetName(
            std::wstring(L"MARS::PathTracer::GIReservoirBuffer[" +
                         std::to_wstring(output_index) + L"]").c_str());

        gi.uav_slot = ctx.allocate_bindless_slot();
        D3D12_UNORDERED_ACCESS_VIEW_DESC gi_uav_desc{};
        gi_uav_desc.Format                      = DXGI_FORMAT_UNKNOWN;
        gi_uav_desc.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
        gi_uav_desc.Buffer.FirstElement         = 0;
        gi_uav_desc.Buffer.NumElements          = static_cast<UINT>(element_count);
        gi_uav_desc.Buffer.StructureByteStride  = k_gi_reservoir_stride;
        gi_uav_desc.Buffer.Flags                = D3D12_BUFFER_UAV_FLAG_NONE;
        D3D12_CPU_DESCRIPTOR_HANDLE gi_cpu = ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
        gi_cpu.ptr += static_cast<SIZE_T>(gi.uav_slot) * ctx.bindless_descriptor_size();
        device->CreateUnorderedAccessView(gi.resource, nullptr, &gi_uav_desc, gi_cpu);
    }
}

// ---------------------------------------------------------------------------
// build_blas
uint32_t PathTracer::build_blas(DeviceContext& ctx,
                                 const GpuMeshBuffer& mesh,
                                 bool allow_update,
                                 bool opaque)
{
    auto* device = ctx.device();

    D3D12_RAYTRACING_GEOMETRY_DESC geom_desc{};
    geom_desc.Type                                 = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    // For alpha-tested geometry (e.g. SpeedTree leaf cards), the geometry
    // must NOT be flagged opaque or the any-hit shader is never invoked and
    // the path tracer cannot discard sub-texel transparent regions.
    geom_desc.Flags                                = opaque
                                                       ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                                                       : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    geom_desc.Triangles.VertexBuffer.StartAddress  = mesh.vertex_buffer_view().BufferLocation;
    geom_desc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex); // full interleaved vertex stride
    geom_desc.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
    geom_desc.Triangles.VertexCount                = mesh.vertex_count();
    geom_desc.Triangles.IndexBuffer                = mesh.index_buffer_view().BufferLocation;
    geom_desc.Triangles.IndexFormat                = DXGI_FORMAT_R32_UINT;
    geom_desc.Triangles.IndexCount                 = mesh.index_count();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs       = 1;
    inputs.pGeometryDescs = &geom_desc;
    inputs.Flags          = allow_update
                                ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
                                : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

    // Allocate scratch buffer (DEFAULT heap, UAV).
    // When allow_update is requested the scratch must also be large enough
    // to serve as an update scratch on every subsequent refit call.
    // UpdateScratchDataSizeInBytes can exceed ScratchDataSizeInBytes on some
    // drivers, so we always allocate the maximum of the two.
    D3D12MA::Allocation* scratch_alloc = nullptr;
    ID3D12Resource*      scratch       = nullptr;
    {
        D3D12MA::ALLOCATION_DESC ad{};  ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = allow_update
                                  ? std::max(prebuild.ScratchDataSizeInBytes,
                                             prebuild.UpdateScratchDataSizeInBytes)
                                  : prebuild.ScratchDataSizeInBytes;
        bd.Height           = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        throw_if_failed(m_allocator->CreateResource(&ad, &bd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, &scratch_alloc, IID_PPV_ARGS(&scratch)),
            "BLAS: create scratch failed");
    }

    // Allocate result buffer (DEFAULT heap, UAV, AS flag)
    D3D12MA::Allocation* result_alloc = nullptr;
    ID3D12Resource*      result       = nullptr;
    {
        D3D12MA::ALLOCATION_DESC ad{};  ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = align_up(prebuild.ResultDataMaxSizeInBytes,
                                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        bd.Height           = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                              D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
        throw_if_failed(m_allocator->CreateResource(&ad, &bd,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
            &result_alloc, IID_PPV_ARGS(&result)),
            "BLAS: create result failed");
    }

    // Build on the copy queue's GPU timeline using a temporary command list on the direct queue
    // (simplest approach for M4; a dedicated build queue is a future optimisation).
    {
        ComPtr<ID3D12CommandAllocator>    alloc_cmd;
        ComPtr<ID3D12GraphicsCommandList6> cmd;
        throw_if_failed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                        IID_PPV_ARGS(&alloc_cmd)), "BLAS: CreateCommandAllocator failed");
        throw_if_failed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                        alloc_cmd.Get(), nullptr, IID_PPV_ARGS(&cmd)), "BLAS: CreateCommandList failed");

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc{};
        build_desc.Inputs                           = inputs;
        build_desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        build_desc.DestAccelerationStructureData    = result->GetGPUVirtualAddress();

        cmd->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);

        D3D12_RESOURCE_BARRIER uav_barrier{};
        uav_barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_barrier.UAV.pResource = result;
        cmd->ResourceBarrier(1, &uav_barrier);

        cmd->Close();
        ID3D12CommandList* lists[] = { cmd.Get() };
        ctx.direct_queue()->ExecuteCommandLists(1, lists);
        ctx.flush_gpu();
    }

    // Free scratch immediately after build — UNLESS allow_update is requested,
    // in which case the scratch must be kept alive for per-frame BLAS refits.
    if (!allow_update)
        scratch_alloc->Release();

    uint32_t index = static_cast<uint32_t>(m_blas_list.size());
    BlasEntry entry{};
    entry.alloc        = result_alloc;
    entry.resource     = result;
    entry.allow_update = allow_update;
    if (allow_update)
    {
        entry.scratch_alloc = scratch_alloc;
        entry.scratch       = scratch;
        entry.vertex_count  = mesh.vertex_count();
        entry.index_count   = mesh.index_count();
    }
    m_blas_list.push_back(entry);
    return index;
}

// ---------------------------------------------------------------------------
// build_skinned_blas
// Like build_blas but keeps a persistent scratch buffer and uses ALLOW_UPDATE
// so the BLAS can be refitted each frame after GPU skinning.
// ---------------------------------------------------------------------------
uint32_t PathTracer::build_skinned_blas(DeviceContext& ctx, const GpuMeshBuffer& mesh, bool opaque)
{
    auto* device = ctx.device();

    // The skinned/deformed vertex buffer must already be allocated by the
    // caller (e.g. via GpuMeshBuffer::enable_skinning). Without it the
    // geometry descriptor below would dereference a null resource pointer.
    if (!mesh.skinned_vertex_buffer())
        throw std::runtime_error(
            "PathTracer::build_skinned_blas: mesh has no skinned vertex buffer "
            "(call GpuMeshBuffer::enable_skinning or enable_wind_deform first)");

    D3D12_RAYTRACING_GEOMETRY_DESC geom_desc{};
    geom_desc.Type                                 = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom_desc.Flags                                = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                                                            : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    geom_desc.Triangles.VertexBuffer.StartAddress  = mesh.skinned_vertex_buffer()->GetGPUVirtualAddress();
    geom_desc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
    geom_desc.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
    geom_desc.Triangles.VertexCount                = mesh.vertex_count();
    geom_desc.Triangles.IndexBuffer                = mesh.index_buffer_view().BufferLocation;
    geom_desc.Triangles.IndexFormat                = DXGI_FORMAT_R32_UINT;
    geom_desc.Triangles.IndexCount                 = mesh.index_count();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs       = 1;
    inputs.pGeometryDescs = &geom_desc;
    inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

    D3D12MA::ALLOCATION_DESC ad{}; ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    // Persistent scratch — kept alive across frames for refit.
    // Must be large enough for both the initial full build (ScratchDataSizeInBytes)
    // and subsequent per-frame update passes (UpdateScratchDataSizeInBytes).
    // UpdateScratchDataSizeInBytes can be smaller than ScratchDataSizeInBytes, so
    // we take the maximum to guarantee the buffer is never undersized for either use.
    D3D12MA::Allocation* scratch_alloc = nullptr;
    ID3D12Resource*      scratch       = nullptr;
    {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = std::max(prebuild.ScratchDataSizeInBytes,
                                       prebuild.UpdateScratchDataSizeInBytes);
        bd.Height           = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        throw_if_failed(m_allocator->CreateResource(&ad, &bd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, &scratch_alloc, IID_PPV_ARGS(&scratch)),
            "SkinnedBLAS: create scratch failed");
    }

    D3D12MA::Allocation* result_alloc = nullptr;
    ID3D12Resource*      result       = nullptr;
    {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = align_up(prebuild.ResultDataMaxSizeInBytes,
                                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        bd.Height           = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                              D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
        throw_if_failed(m_allocator->CreateResource(&ad, &bd,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
            &result_alloc, IID_PPV_ARGS(&result)),
            "SkinnedBLAS: create result failed");
    }

    // Initial full build using a temporary command list.
    // The persistent scratch is already sized for ScratchDataSizeInBytes, so
    // it can be reused directly here — no separate temporary allocation needed.
    {
        ComPtr<ID3D12CommandAllocator>     alloc_cmd;
        ComPtr<ID3D12GraphicsCommandList6> cmd;
        throw_if_failed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                        IID_PPV_ARGS(&alloc_cmd)), "SkinnedBLAS: CreateCommandAllocator failed");
        throw_if_failed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                        alloc_cmd.Get(), nullptr, IID_PPV_ARGS(&cmd)), "SkinnedBLAS: CreateCommandList failed");

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc{};
        build_desc.Inputs                           = inputs;
        build_desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        build_desc.DestAccelerationStructureData    = result->GetGPUVirtualAddress();

        cmd->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);

        D3D12_RESOURCE_BARRIER uav_barrier{};
        uav_barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_barrier.UAV.pResource = result;
        cmd->ResourceBarrier(1, &uav_barrier);
        cmd->Close();

        ID3D12CommandList* lists[] = { cmd.Get() };
        ctx.direct_queue()->ExecuteCommandLists(1, lists);
        ctx.flush_gpu();
    }

    BlasEntry entry{};
    entry.alloc            = result_alloc;
    entry.resource         = result;
    entry.scratch_alloc    = scratch_alloc;
    entry.scratch          = scratch;
    entry.allow_update     = true;
    entry.geometry_opaque  = opaque;
    entry.vertex_count     = mesh.vertex_count();
    entry.index_count      = mesh.index_count();

    uint32_t index = static_cast<uint32_t>(m_blas_list.size());
    m_blas_list.push_back(entry);
    return index;
}

// ---------------------------------------------------------------------------
// build_vegetation_lod_blas
// Build an ALLOW_UPDATE BLAS for a single vegetation LOD mesh. The wind
// compute pass will refit this BLAS each frame using the deformed vertex
// buffer produced by dispatch_vegetation_wind(). Geometry layout is the
// engine's standard triangle mesh, so we delegate to build_skinned_blas()
// which already implements the persistent-scratch + ALLOW_UPDATE flow.
// ---------------------------------------------------------------------------
uint32_t PathTracer::build_vegetation_lod_blas(DeviceContext& ctx,
                                                const GpuMeshBuffer& mesh,
                                                bool opaque)
{
    const uint32_t blas_index = build_skinned_blas(ctx, mesh, opaque);
    MARS_LOG("[PathTracer] build_vegetation_lod_blas: blas_index={} vtx={} idx={}",
             blas_index, mesh.vertex_count(), mesh.index_count());
    return blas_index;
}

// ---------------------------------------------------------------------------
// build_vegetation_model_blas
// Build a static BLAS over every submesh of a vegetation LOD model. SpeedTree
// FBX exports break a single tree into many submeshes (trunk / branches /
// leaves / fronds), so the BLAS must include all of them or the rendered
// geometry will look like a degenerate spike of whichever submesh happened
// to be ordered first.
// ---------------------------------------------------------------------------
uint32_t PathTracer::build_vegetation_model_blas(DeviceContext& ctx,
                                                  const std::vector<GpuMeshBuffer>& meshes)
{
    auto* device = ctx.device();

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geom_descs;
    geom_descs.reserve(meshes.size());

    uint64_t total_vtx = 0;
    uint64_t total_idx = 0;

    for (const auto& mesh : meshes)
    {
        if (mesh.vertex_count() == 0 || mesh.index_count() == 0)
            continue;

        D3D12_RAYTRACING_GEOMETRY_DESC gd{};
        gd.Type                                 = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        gd.Flags                                = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        gd.Triangles.VertexBuffer.StartAddress  = mesh.vertex_buffer_view().BufferLocation;
        gd.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
        gd.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
        gd.Triangles.VertexCount                = mesh.vertex_count();
        gd.Triangles.IndexBuffer                = mesh.index_buffer_view().BufferLocation;
        gd.Triangles.IndexFormat                = DXGI_FORMAT_R32_UINT;
        gd.Triangles.IndexCount                 = mesh.index_count();
        geom_descs.push_back(gd);

        total_vtx += mesh.vertex_count();
        total_idx += mesh.index_count();
    }

    if (geom_descs.empty())
    {
        MARS_LOG("[PathTracer] build_vegetation_model_blas: no valid submeshes");
        return UINT32_MAX;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs       = static_cast<UINT>(geom_descs.size());
    inputs.pGeometryDescs = geom_descs.data();
    inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

    D3D12MA::ALLOCATION_DESC ad{}; ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12MA::Allocation* scratch_alloc = nullptr;
    ID3D12Resource*      scratch       = nullptr;
    {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = prebuild.ScratchDataSizeInBytes;
        bd.Height           = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        throw_if_failed(m_allocator->CreateResource(&ad, &bd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, &scratch_alloc, IID_PPV_ARGS(&scratch)),
            "VegetationModelBLAS: create scratch failed");
    }

    D3D12MA::Allocation* result_alloc = nullptr;
    ID3D12Resource*      result       = nullptr;
    {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = align_up(prebuild.ResultDataMaxSizeInBytes,
                                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        bd.Height           = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                              D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
        throw_if_failed(m_allocator->CreateResource(&ad, &bd,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
            &result_alloc, IID_PPV_ARGS(&result)),
            "VegetationModelBLAS: create result failed");
    }

    {
        ComPtr<ID3D12CommandAllocator>     alloc_cmd;
        ComPtr<ID3D12GraphicsCommandList6> cmd;
        throw_if_failed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                        IID_PPV_ARGS(&alloc_cmd)), "VegetationModelBLAS: CreateCommandAllocator failed");
        throw_if_failed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                        alloc_cmd.Get(), nullptr, IID_PPV_ARGS(&cmd)), "VegetationModelBLAS: CreateCommandList failed");

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc{};
        build_desc.Inputs                           = inputs;
        build_desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        build_desc.DestAccelerationStructureData    = result->GetGPUVirtualAddress();

        cmd->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);

        D3D12_RESOURCE_BARRIER uav_barrier{};
        uav_barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_barrier.UAV.pResource = result;
        cmd->ResourceBarrier(1, &uav_barrier);
        cmd->Close();

        ID3D12CommandList* lists[] = { cmd.Get() };
        ctx.direct_queue()->ExecuteCommandLists(1, lists);
        ctx.flush_gpu();
    }

    // Scratch is not needed past the build (no ALLOW_UPDATE).
    scratch_alloc->Release();
    (void)scratch;

    BlasEntry entry{};
    entry.alloc        = result_alloc;
    entry.resource     = result;
    entry.allow_update = false;

    uint32_t index = static_cast<uint32_t>(m_blas_list.size());
    m_blas_list.push_back(entry);

    MARS_LOG("[PathTracer] build_vegetation_model_blas: blas_index={} submeshes={} total_vtx={} total_idx={}",
             index, geom_descs.size(), total_vtx, total_idx);
    return index;
}

// ---------------------------------------------------------------------------
// build_vegetation_impostor_blas
// Build a procedural-AABB BLAS for the Impostor LOD. A single AABB primitive
// is uploaded; the path tracer's custom intersection shader is responsible
// for sampling the octahedral impostor atlas and producing the final hit.
// ---------------------------------------------------------------------------
uint32_t PathTracer::build_vegetation_impostor_blas(DeviceContext& ctx,
                                                     const Vec3& aabb_min,
                                                     const Vec3& aabb_max)
{
    auto* device = ctx.device();

    // ---- Upload the single AABB into a tiny default-heap buffer -----------
    D3D12_RAYTRACING_AABB aabb{};
    aabb.MinX = aabb_min.x; aabb.MinY = aabb_min.y; aabb.MinZ = aabb_min.z;
    aabb.MaxX = aabb_max.x; aabb.MaxY = aabb_max.y; aabb.MaxZ = aabb_max.z;

    D3D12MA::Allocation* aabb_alloc = nullptr;
    ID3D12Resource*      aabb_buf   = nullptr;
    {
        D3D12MA::ALLOCATION_DESC ad{}; ad.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = sizeof(D3D12_RAYTRACING_AABB);
        bd.Height           = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        throw_if_failed(m_allocator->CreateResource(&ad, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            &aabb_alloc, IID_PPV_ARGS(&aabb_buf)),
            "ImpostorBLAS: create AABB upload buffer failed");

        void* mapped = nullptr;
        throw_if_failed(aabb_buf->Map(0, nullptr, &mapped), "ImpostorBLAS: Map failed");
        std::memcpy(mapped, &aabb, sizeof(aabb));
        aabb_buf->Unmap(0, nullptr);
    }

    D3D12_RAYTRACING_GEOMETRY_DESC geom_desc{};
    geom_desc.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    geom_desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom_desc.AABBs.AABBCount               = 1;
    geom_desc.AABBs.AABBs.StartAddress      = aabb_buf->GetGPUVirtualAddress();
    geom_desc.AABBs.AABBs.StrideInBytes     = sizeof(D3D12_RAYTRACING_AABB);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs       = 1;
    inputs.pGeometryDescs = &geom_desc;
    inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

    // ---- Scratch ----------------------------------------------------------
    D3D12MA::Allocation* scratch_alloc = nullptr;
    ID3D12Resource*      scratch       = nullptr;
    {
        D3D12MA::ALLOCATION_DESC ad{}; ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = prebuild.ScratchDataSizeInBytes;
        bd.Height           = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        throw_if_failed(m_allocator->CreateResource(&ad, &bd,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            &scratch_alloc, IID_PPV_ARGS(&scratch)),
            "ImpostorBLAS: scratch alloc failed");
    }

    // ---- Result -----------------------------------------------------------
    D3D12MA::Allocation* result_alloc = nullptr;
    ID3D12Resource*      result       = nullptr;
    {
        D3D12MA::ALLOCATION_DESC ad{}; ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = align_up(prebuild.ResultDataMaxSizeInBytes,
                                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        bd.Height           = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                              D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
        throw_if_failed(m_allocator->CreateResource(&ad, &bd,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
            &result_alloc, IID_PPV_ARGS(&result)),
            "ImpostorBLAS: result alloc failed");
    }

    // ---- Build ------------------------------------------------------------
    {
        ComPtr<ID3D12CommandAllocator>     alloc_cmd;
        ComPtr<ID3D12GraphicsCommandList6> cmd;
        throw_if_failed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                        IID_PPV_ARGS(&alloc_cmd)), "ImpostorBLAS: CreateCommandAllocator failed");
        throw_if_failed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                        alloc_cmd.Get(), nullptr, IID_PPV_ARGS(&cmd)),
                        "ImpostorBLAS: CreateCommandList failed");

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
        bd.Inputs                           = inputs;
        bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        bd.DestAccelerationStructureData    = result->GetGPUVirtualAddress();
        cmd->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);

        D3D12_RESOURCE_BARRIER uav_barrier{};
        uav_barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_barrier.UAV.pResource = result;
        cmd->ResourceBarrier(1, &uav_barrier);

        cmd->Close();
        ID3D12CommandList* lists[] = { cmd.Get() };
        ctx.direct_queue()->ExecuteCommandLists(1, lists);
        ctx.flush_gpu();
    }

    // Scratch + upload buffer are no longer needed; release them.
    scratch_alloc->Release();
    aabb_alloc->Release();

    BlasEntry entry{};
    entry.alloc        = result_alloc;
    entry.resource     = result;
    entry.allow_update = false;
    entry.vertex_count = 0;
    entry.index_count  = 0;
    entry.is_impostor  = true;

    const uint32_t index = static_cast<uint32_t>(m_blas_list.size());
    m_blas_list.push_back(entry);

    MARS_LOG("[PathTracer] build_vegetation_impostor_blas: blas_index={} aabb=({} {} {})-({} {} {})",
             index, aabb_min.x, aabb_min.y, aabb_min.z, aabb_max.x, aabb_max.y, aabb_max.z);

    return index;
}

// ---------------------------------------------------------------------------
// refit_blas
// Issues an in-place BLAS update (refit) for a skinned mesh using the
// skinned vertex buffer written by dispatch_skinning().
// Call this after dispatch_skinning() and a UAV barrier, before build_tlas().
// ---------------------------------------------------------------------------
void PathTracer::refit_blas(DeviceContext& /*ctx*/,
                             ID3D12GraphicsCommandList6* cmd_list,
                             uint32_t blas_index,
                             const GpuMeshBuffer& mesh)
{
    assert(blas_index < m_blas_list.size());
    BlasEntry& entry = m_blas_list[blas_index];
    assert(entry.allow_update && entry.scratch);

    D3D12_RAYTRACING_GEOMETRY_DESC geom_desc{};
    geom_desc.Type                                 = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom_desc.Flags                                = entry.geometry_opaque
                                                       ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                                                       : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    geom_desc.Triangles.VertexBuffer.StartAddress  = mesh.skinned_vertex_buffer()->GetGPUVirtualAddress();
    geom_desc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
    geom_desc.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
    geom_desc.Triangles.VertexCount                = entry.vertex_count;
    geom_desc.Triangles.IndexBuffer                = mesh.index_buffer_view().BufferLocation;
    geom_desc.Triangles.IndexFormat                = DXGI_FORMAT_R32_UINT;
    geom_desc.Triangles.IndexCount                 = entry.index_count;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs       = 1;
    inputs.pGeometryDescs = &geom_desc;
    inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC refit_desc{};
    refit_desc.Inputs                           = inputs;
    refit_desc.ScratchAccelerationStructureData = entry.scratch->GetGPUVirtualAddress();
    refit_desc.SourceAccelerationStructureData  = entry.resource->GetGPUVirtualAddress();
    refit_desc.DestAccelerationStructureData    = entry.resource->GetGPUVirtualAddress();

    cmd_list->BuildRaytracingAccelerationStructure(&refit_desc, 0, nullptr);

    D3D12_RESOURCE_BARRIER uav_barrier{};
    uav_barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barrier.UAV.pResource = entry.resource;
    cmd_list->ResourceBarrier(1, &uav_barrier);
}

// ---------------------------------------------------------------------------
// refit_blas (cloth overload)
// Same as the GpuMeshBuffer overload but accepts an explicit vertex resource.
// Used by cloth instances whose deformed vertices live in ClothGpuResources.
// ---------------------------------------------------------------------------
void PathTracer::refit_blas(DeviceContext& /*ctx*/,
                             ID3D12GraphicsCommandList6* cmd_list,
                             uint32_t blas_index,
                             ID3D12Resource* vertex_resource,
                             uint32_t vertex_count,
                             uint32_t index_count,
                             ID3D12Resource* index_resource)
{
    assert(blas_index < m_blas_list.size());
    BlasEntry& entry = m_blas_list[blas_index];
    assert(entry.allow_update && entry.scratch);

    static bool s_logged = false;
    if (!s_logged)
    {
        s_logged = true;
        MARS_LOG("[PathTracer] refit_blas(cloth): blas_index={} vtx_res={} vtx_count={}"
                     " idx_res={} idx_count={} scratch={} result={}",
                     blas_index,
                     static_cast<void*>(vertex_resource), vertex_count,
                     static_cast<void*>(index_resource),  index_count,
                     static_cast<void*>(entry.scratch),
                     static_cast<void*>(entry.resource));
    }

    D3D12_RAYTRACING_GEOMETRY_DESC geom_desc{};
    geom_desc.Type                                 = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom_desc.Flags                                = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom_desc.Triangles.VertexBuffer.StartAddress  = vertex_resource->GetGPUVirtualAddress();
    geom_desc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
    geom_desc.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
    geom_desc.Triangles.VertexCount                = vertex_count;
    geom_desc.Triangles.IndexBuffer                = index_resource->GetGPUVirtualAddress();
    geom_desc.Triangles.IndexFormat                = DXGI_FORMAT_R32_UINT;
    geom_desc.Triangles.IndexCount                 = index_count;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs       = 1;
    inputs.pGeometryDescs = &geom_desc;
    inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC refit_desc{};
    refit_desc.Inputs                           = inputs;
    refit_desc.ScratchAccelerationStructureData = entry.scratch->GetGPUVirtualAddress();
    refit_desc.SourceAccelerationStructureData  = entry.resource->GetGPUVirtualAddress();
    refit_desc.DestAccelerationStructureData    = entry.resource->GetGPUVirtualAddress();

    cmd_list->BuildRaytracingAccelerationStructure(&refit_desc, 0, nullptr);

    D3D12_RESOURCE_BARRIER uav_barrier{};
    uav_barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barrier.UAV.pResource = entry.resource;
    cmd_list->ResourceBarrier(1, &uav_barrier);
}
// Uploads a CPU bone palette to the mesh's bone palette upload buffer and
// dispatches the skinning compute shader to produce skinned vertices.
// Call this before refit_blas() on the same command list.
// ---------------------------------------------------------------------------
void PathTracer::dispatch_skinning(ID3D12GraphicsCommandList6* cmd_list,
                                    const GpuMeshBuffer& mesh,
                                    const std::vector<Mat4x4>& bone_palette)
{
    assert(mesh.is_skinned());
    assert(!bone_palette.empty());

    // Upload bone palette to the mesh's CPU-writable upload buffer.
    mesh.upload_bone_palette(bone_palette);

    // Bind the skinning pipeline.
    cmd_list->SetComputeRootSignature(m_skinning_root_sig.Get());
    cmd_list->SetPipelineState(m_skinning_pso.Get());

    // Root constants: [vertex_count, src_vertex_srv, bone_palette_srv, dst_vertex_uav]
    const uint32_t constants[4] = {
        mesh.vertex_count(),
        mesh.vertex_srv_slot(),
        mesh.bone_palette_srv_slot(),
        mesh.skinned_vertex_uav_slot()
    };
    cmd_list->SetComputeRoot32BitConstants(0, 4, constants, 0);

    // Dispatch — 64 threads per group (matches [numthreads(64,1,1)] in skinning.hlsl)
    constexpr uint32_t k_threads = 64;
    const uint32_t groups = (mesh.vertex_count() + k_threads - 1) / k_threads;
    cmd_list->Dispatch(groups, 1, 1);

    // UAV barrier so the skinned vertex buffer is ready for BLAS refit.
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = mesh.skinned_vertex_buffer();
    cmd_list->ResourceBarrier(1, &uav);
}
void PathTracer::set_instance(uint32_t instance_index,
                               uint32_t blas_index,
                               const Mat4x4& transform,
                               uint32_t material_index)
{
    if (instance_index >= m_instances.size())
        m_instances.resize(instance_index + 1);

    m_instances[instance_index] = { blas_index, material_index, transform };
}

// ---------------------------------------------------------------------------
// build_tlas
// ---------------------------------------------------------------------------
void PathTracer::build_tlas(DeviceContext& ctx,
                             ID3D12GraphicsCommandList6* cmd_list,
                             uint32_t frame_index,
                             bool allow_update)
{
    auto* device = ctx.device();

    uint32_t instance_count = static_cast<uint32_t>(m_instances.size());
    if (instance_count == 0)
    {
        MARS_LOG("[PathTracer] build_tlas: instance_count == 0 — TLAS not built, tlas_srv_slot stays UINT32_MAX.");
        return;
    }
    if (frame_index < 3)
        MARS_LOG("[PathTracer] build_tlas: building TLAS for {} instance(s).", instance_count);

    // Upload instance descriptors to an upload buffer
    uint64_t instance_buf_size = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instance_count;

    if (!m_instance_buf_alloc)
    {
        void* mapped = nullptr;
        create_upload_buffer(ctx, instance_buf_size,
                             L"MARS::TLAS::InstanceBuffer",
                             &m_instance_buf_alloc, &m_instance_buffer, &mapped);
    }

    // Fill instance descriptors
    void* instance_mapped = nullptr;
    m_instance_buffer->Map(0, nullptr, &instance_mapped);
    auto* instance_descs = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(instance_mapped);

    for (uint32_t i = 0; i < instance_count; ++i)
    {
        const InstanceDesc& inst = m_instances[i];
        D3D12_RAYTRACING_INSTANCE_DESC& d = instance_descs[i];
        ZeroMemory(&d, sizeof(d));

        // D3D12 stores the transform as a 3×4 row-major matrix (no bottom row).
        // Our Mat4x4 is row-major so we just copy the first 3 rows.
        const float (&src)[4][4] = inst.transform.m;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                d.Transform[r][c] = src[r][c];

        d.InstanceID                          = i;
        d.InstanceMask                        = 0xFF;
        d.InstanceContributionToHitGroupIndex =
            m_blas_list[inst.blas_index].is_impostor ? 1u : 0u;
        d.Flags                               = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        d.AccelerationStructure               = m_blas_list[inst.blas_index].resource->GetGPUVirtualAddress();

        static uint32_t s_tlas_log_count = 0;
        if (s_tlas_log_count < instance_count)
        {
            ++s_tlas_log_count;
            MARS_LOG("[PathTracer] build_tlas: inst[{}] blas_index={} mat={} gpu_va={:#018x} allow_update={}",
                         i, inst.blas_index, inst.material_index,
                         d.AccelerationStructure,
                         m_blas_list[inst.blas_index].allow_update);
        }
    }
    m_instance_buffer->Unmap(0, nullptr);

    // Get prebuild info
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs      = instance_count;
    inputs.InstanceDescs = m_instance_buffer->GetGPUVirtualAddress();
    inputs.Flags         = allow_update
                               ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
                               : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

    // (Re)allocate scratch and result if not yet created or too small
    auto ensure_buffer = [&](D3D12MA::Allocation*& alloc, ID3D12Resource*& res,
                              uint64_t needed_size, bool is_as, const wchar_t* name)
    {
        uint64_t current_size = res ? res->GetDesc().Width : 0;
        if (current_size >= needed_size) return;
        if (alloc) { alloc->Release(); alloc = nullptr; res = nullptr; }

        D3D12MA::ALLOCATION_DESC ad{};  ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = needed_size;
        bd.Height           = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                              (is_as ? D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE
                                     : D3D12_RESOURCE_FLAG_NONE);
        D3D12_RESOURCE_STATES state = is_as
            ? D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
            : D3D12_RESOURCE_STATE_COMMON;

        throw_if_failed(m_allocator->CreateResource(&ad, &bd, state, nullptr, &alloc, IID_PPV_ARGS(&res)),
                        "TLAS: create buffer failed");
        res->SetName(name);
    };

    ensure_buffer(m_tlas_scratch_alloc, m_tlas_scratch,
                  prebuild.ScratchDataSizeInBytes, false, L"MARS::TLAS::Scratch");
    ensure_buffer(m_tlas_alloc, m_tlas_resource,
                  prebuild.ResultDataMaxSizeInBytes, true, L"MARS::TLAS");

    // Build
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc{};
    build_desc.Inputs                           = inputs;
    build_desc.ScratchAccelerationStructureData = m_tlas_scratch->GetGPUVirtualAddress();
    build_desc.DestAccelerationStructureData    = m_tlas_resource->GetGPUVirtualAddress();

    cmd_list->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);

    D3D12_RESOURCE_BARRIER uav_barrier{};
    uav_barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barrier.UAV.pResource = m_tlas_resource;
    cmd_list->ResourceBarrier(1, &uav_barrier);

    // Register TLAS in bindless heap as an SRV (acceleration structure view)
    if (m_tlas_srv_slot == UINT32_MAX)
        m_tlas_srv_slot = ctx.allocate_bindless_slot();

    if (frame_index < 3)
        MARS_LOG("[PathTracer] build_tlas: TLAS built successfully. tlas_srv_slot = {}.", m_tlas_srv_slot);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format                                   = DXGI_FORMAT_UNKNOWN;
    srv_desc.ViewDimension                            = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srv_desc.Shader4ComponentMapping                  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.RaytracingAccelerationStructure.Location = m_tlas_resource->GetGPUVirtualAddress();

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
    cpu_handle.ptr += static_cast<SIZE_T>(m_tlas_srv_slot) * ctx.bindless_descriptor_size();
    device->CreateShaderResourceView(nullptr, &srv_desc, cpu_handle);
}

// ---------------------------------------------------------------------------
// upload_scene_buffers
// Builds GPU-side structured buffers for per-instance and per-material data
// and registers them as bindless SRVs so the path-trace shader can fetch them.
// ---------------------------------------------------------------------------
void PathTracer::upload_scene_buffers(DeviceContext& ctx,
                                       const std::vector<CpuInstanceData>& instances,
                                       const std::vector<CpuMaterialData>& materials)
{
    auto* device = ctx.device();

    // Helper: create an UPLOAD-heap buffer, copy data into it, then copy to a
    // DEFAULT-heap buffer, register an SRV, and return the slot.
    auto upload_structured = [&](const void*  data,
                                  uint32_t     element_count,
                                  uint32_t     element_stride,
                                  const wchar_t* name,
                                  D3D12MA::Allocation*& out_alloc,
                                  ID3D12Resource*&      out_resource,
                                  uint32_t&             out_srv_slot) -> bool
    {
        if (element_count == 0) return false;

        uint64_t byte_size = static_cast<uint64_t>(element_count) * element_stride;

        // Release previous if any
        if (out_alloc) { out_alloc->Release(); out_alloc = nullptr; out_resource = nullptr; }

        // Create DEFAULT-heap buffer
        D3D12MA::ALLOCATION_DESC ad{};
        ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = byte_size;
        bd.Height           = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags            = D3D12_RESOURCE_FLAG_NONE;

        throw_if_failed(m_allocator->CreateResource(&ad, &bd,
                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                         &out_alloc, IID_PPV_ARGS(&out_resource)),
                        "upload_scene_buffers: create DEFAULT buffer failed");
        out_resource->SetName(name);

        // Upload via UPLOAD-heap staging
        D3D12MA::Allocation* stage_alloc = nullptr;
        ID3D12Resource*      stage_res   = nullptr;
        void*                stage_ptr   = nullptr;
        create_upload_buffer(ctx, byte_size, L"MARS::SceneBuffer::Staging",
                             &stage_alloc, &stage_res, &stage_ptr);
        memcpy(stage_ptr, data, static_cast<size_t>(byte_size));
        stage_res->Unmap(0, nullptr);

        // One-shot command list for copy
        ComPtr<ID3D12CommandAllocator>        copy_alloc;
        ComPtr<ID3D12GraphicsCommandList>     copy_cmd;
        throw_if_failed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                         IID_PPV_ARGS(&copy_alloc)), "CreateCommandAllocator (scene buf)");
        throw_if_failed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                         copy_alloc.Get(), nullptr, IID_PPV_ARGS(&copy_cmd)),
                        "CreateCommandList (scene buf)");

        copy_cmd->CopyBufferRegion(out_resource, 0, stage_res, 0, byte_size);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = out_resource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copy_cmd->ResourceBarrier(1, &barrier);

        throw_if_failed(copy_cmd->Close(), "CommandList::Close (scene buf)");
        ID3D12CommandList* lists[] = { copy_cmd.Get() };
        ctx.direct_queue()->ExecuteCommandLists(1, lists);
        ctx.flush_gpu();

        stage_alloc->Release();

        // Register SRV
        if (out_srv_slot == UINT32_MAX)
            out_srv_slot = ctx.allocate_bindless_slot();

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format                     = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement        = 0;
        srv.Buffer.NumElements         = element_count;
        srv.Buffer.StructureByteStride = element_stride;
        srv.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle =
            ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
        cpu_handle.ptr += static_cast<SIZE_T>(out_srv_slot) * ctx.bindless_descriptor_size();
        device->CreateShaderResourceView(out_resource, &srv, cpu_handle);

        return true;
    };

    upload_structured(instances.data(),
                      static_cast<uint32_t>(instances.size()),
                      static_cast<uint32_t>(sizeof(CpuInstanceData)),
                      L"MARS::InstanceDataBuffer",
                      m_instance_data_alloc, m_instance_data_buffer, m_instance_data_srv_slot);

    upload_structured(materials.data(),
                      static_cast<uint32_t>(materials.size()),
                      static_cast<uint32_t>(sizeof(CpuMaterialData)),
                      L"MARS::MaterialDataBuffer",
                      m_material_data_alloc, m_material_data_buffer, m_material_data_srv_slot);

    MARS_LOG("[PathTracer] upload_scene_buffers: instance_srv={} material_srv={}",
                 m_instance_data_srv_slot, m_material_data_srv_slot);
}

// ---------------------------------------------------------------------------
// update_frame_constants
// ---------------------------------------------------------------------------
void PathTracer::update_frame_constants(DeviceContext& ctx,
                                         uint32_t output_index,
                                         uint32_t frame_index,
                                         const Vec3& camera_pos,
                                         const Mat4x4& view_inv,
                                         const Mat4x4& proj_inv,
                                         const Vec3& sun_direction,
                                         const Vec3& sun_color,
                                         float sun_intensity,
                                         const Mat4x4& prev_view_proj)
{
    (void)ctx;
    uint32_t slot_idx = output_index * k_frame_count + (frame_index % k_frame_count);
    FrameCBSlot& slot = m_frame_cbs[slot_idx];

    const OutputTexture& out    = m_outputs[output_index];
    const OutputTexture& mv_out = m_mv_outputs[output_index];
    const OutputTexture& d_out  = m_depth_outputs[output_index];

    FrameConstants fc{};
    fc.view_inv                 = view_inv;
    fc.proj_inv                 = proj_inv;
    fc.camera_pos               = camera_pos;
    fc.frame_index              = frame_index;
    fc.output_width             = out.width;
    fc.output_height            = out.height;
    fc.tlas_slot                = m_tlas_srv_slot;
    fc.material_buffer_slot     = m_material_data_srv_slot;
    fc.instance_buffer_slot     = m_instance_data_srv_slot;
    fc.output_uav_slot          = out.uav_slot;
    fc.motion_vector_uav_slot   = mv_out.uav_slot;
    fc.depth_uav_slot           = d_out.uav_slot;
    fc.albedo_uav_slot          = m_albedo_outputs[output_index].uav_slot;
    fc.specular_albedo_uav_slot = m_specular_albedo_outputs[output_index].uav_slot;
    fc.normals_uav_slot         = m_normals_aov_outputs[output_index].uav_slot;
    fc.roughness_uav_slot       = m_roughness_aov_outputs[output_index].uav_slot;
    fc.gi_reservoir_uav_slot    = (output_index < m_gi_reservoir_buffers.size())
                                      ? m_gi_reservoir_buffers[output_index].uav_slot
                                      : UINT32_MAX;
    fc.gi_bounce_count          = m_gi_bounce_count;
    fc.sun_direction            = sun_direction;
    fc.sun_intensity            = sun_intensity;
    fc.sun_color                = sun_color;
    fc.prev_view_proj           = prev_view_proj;

    // Compute proj * view (world→clip) for depth and motion-vector projection in the shader.
    // Must be proj * view, not view * proj: the shader does mul(view_proj, worldPos)
    // which is view_proj × pos in column-vector convention.
    Mat4x4 view = view_inv.inverse();
    Mat4x4 proj = proj_inv.inverse();
    fc.view_proj = proj * view;

    memcpy(slot.mapped_ptr, &fc, sizeof(FrameConstants));
}

// ---------------------------------------------------------------------------
// trace
// ---------------------------------------------------------------------------
void PathTracer::trace(ID3D12GraphicsCommandList6* cmd_list,
                       uint32_t output_index,
                       uint32_t frame_index)
{
    if (!m_initialised || m_tlas_srv_slot == UINT32_MAX)
    {
        // Log only on the first call so we don't spam every frame.
        static bool s_logged = false;
        if (!s_logged)
        {
            s_logged = true;
            MARS_LOG("[PathTracer] trace() early-return: initialised={} tlas_srv_slot={}",
                         m_initialised ? "true" : "false", m_tlas_srv_slot);
        }
        return;
    }

    const OutputTexture& out = m_outputs[output_index];
    if (out.width == 0 || out.height == 0 || out.resource == nullptr)
    {
        static bool s_logged_out = false;
        if (!s_logged_out)
        {
            s_logged_out = true;
            MARS_LOG("[PathTracer] trace() early-return: output[{}] not ready ({}x{}, resource={})",
                         output_index, out.width, out.height,
                         out.resource ? "valid" : "null");
        }
        return;
    }

    // Bind RTPSO
    cmd_list->SetPipelineState1(m_rtpso.Get());

    // Bind global root signature
    cmd_list->SetComputeRootSignature(m_global_root_sig.Get());

    // Set the per-frame CB (root param 0)
    uint32_t slot_idx = output_index * k_frame_count + (frame_index % k_frame_count);
    cmd_list->SetComputeRootConstantBufferView(0,
        m_frame_cbs[slot_idx].resource->GetGPUVirtualAddress());

    // Bind the bindless heap (root param 1 — covers all SRV/UAV spaces)
    cmd_list->SetComputeRootDescriptorTable(1, m_bindless_heap_gpu_start);

    // Dispatch rays
    D3D12_DISPATCH_RAYS_DESC rays{};

    // Ray-gen
    rays.RayGenerationShaderRecord.StartAddress = m_raygen_table->GetGPUVirtualAddress();
    rays.RayGenerationShaderRecord.SizeInBytes  = k_raygen_record_stride;

    // Miss (2 shaders)
    rays.MissShaderTable.StartAddress  = m_miss_table->GetGPUVirtualAddress();
    rays.MissShaderTable.SizeInBytes   = k_miss_record_stride * 2;
    rays.MissShaderTable.StrideInBytes = k_miss_record_stride;

    // Hit group
    rays.HitGroupTable.StartAddress  = m_hitgroup_table->GetGPUVirtualAddress();
    rays.HitGroupTable.SizeInBytes   = k_hitgroup_record_stride * 2;
    rays.HitGroupTable.StrideInBytes = k_hitgroup_record_stride;

    rays.Width  = out.width;
    rays.Height = out.height;
    rays.Depth  = 1;

    cmd_list->DispatchRays(&rays);
}

// ---------------------------------------------------------------------------
// copy_to_back_buffer
// Tone-maps and blits the RGBA16F path-tracer output into `back_buffer`.
// `back_buffer` must already be in D3D12_RESOURCE_STATE_RENDER_TARGET.
// `rtv`               — CPU handle for the back-buffer render target view.
// `hdr_mode`          — 0 SDR / 1 HDR10 / 2 scRGB.
// `back_buffer_format`— swap-chain format; selects the correct PSO.
// ---------------------------------------------------------------------------
void PathTracer::copy_to_back_buffer(ID3D12GraphicsCommandList6* cmd_list,
                                      uint32_t output_index,
                                      ID3D12Resource* back_buffer,
                                      D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                      uint32_t hdr_mode,
                                      DXGI_FORMAT back_buffer_format,
                                      bool use_denoised)
{
    // When the denoiser has run, blit from the denoised output; otherwise from
    // the raw path-tracer UAV (no denoiser or denoiser not initialised).
    OutputTexture& raw_out = m_outputs[output_index];
    OutputTexture& dn_out  = m_denoised_outputs[output_index];
    OutputTexture& src_out = (use_denoised && dn_out.resource) ? dn_out : raw_out;

    if (!src_out.resource) return;

    (void)back_buffer; // resource identity unused; caller already transitioned it via RTV

    // Streamline (DLSS-RR) transitions the denoised output via the Enhanced Barrier
    // API internally. Issuing a legacy ResourceBarrier transition on a resource that
    // was last touched by an Enhanced Barrier causes RESOURCE_BARRIER_INVALID_COMBINATION
    // (#526) and crashes ExecuteCommandLists. Skip the legacy barriers when blitting
    // from the Streamline-owned denoised output; the resource is already in a readable
    // state after slEvaluateFeature returns.
    D3D12_RESOURCE_BARRIER to_srv{};
    if (!use_denoised)
    {
        to_srv.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_srv.Transition.pResource   = src_out.resource;
        to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        to_srv.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        to_srv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd_list->ResourceBarrier(1, &to_srv);
    }

    // Select PSO for the swap-chain format.
    ID3D12PipelineState* pso = nullptr;
    switch (back_buffer_format)
    {
    case DXGI_FORMAT_R10G10B10A2_UNORM:  pso = m_blit_pso_hdr10.Get(); break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: pso = m_blit_pso_scrgb.Get(); break;
    default:                             pso = m_blit_pso_sdr.Get();   break;
    }

    cmd_list->SetPipelineState(pso);
    cmd_list->SetGraphicsRootSignature(m_blit_root_sig.Get());

    // Root param 0: two 32-bit constants (src_texture_slot, hdr_mode).
    uint32_t root_consts[2] = { src_out.srv_slot, hdr_mode };
    cmd_list->SetGraphicsRoot32BitConstants(0, 2, root_consts, 0);

    // Root param 1: bindless SRV heap — point at slot 0 (the table covers all slots).
    // The heap was already bound by the caller (render_frame_path_traced).
    cmd_list->SetGraphicsRootDescriptorTable(1, m_bindless_heap_gpu_start);

    // Bind RTV; no depth buffer.
    cmd_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT vp{ 0.f, 0.f,
        static_cast<float>(src_out.width), static_cast<float>(src_out.height),
        0.f, 1.f };
    D3D12_RECT scissor{ 0, 0,
        static_cast<LONG>(src_out.width), static_cast<LONG>(src_out.height) };
    cmd_list->RSSetViewports(1, &vp);
    cmd_list->RSSetScissorRects(1, &scissor);

    cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd_list->DrawInstanced(3, 1, 0, 0);  // full-screen triangle; no vertex buffer

    // Restore UAV state for the next frame — only for resources not managed by Streamline.
    if (!use_denoised)
    {
        std::swap(to_srv.Transition.StateBefore, to_srv.Transition.StateAfter);
        cmd_list->ResourceBarrier(1, &to_srv);
    }
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
ID3D12Resource* PathTracer::output_resource(uint32_t output_index) const
{
    if (output_index < m_outputs.size())
        return m_outputs[output_index].resource;
    return nullptr;
}

ID3D12Resource* PathTracer::motion_vector_resource(uint32_t output_index) const
{
    if (output_index < m_mv_outputs.size())
        return m_mv_outputs[output_index].resource;
    return nullptr;
}

ID3D12Resource* PathTracer::depth_resource(uint32_t output_index) const
{
    if (output_index < m_depth_outputs.size())
        return m_depth_outputs[output_index].resource;
    return nullptr;
}

ID3D12Resource* PathTracer::denoised_output_resource(uint32_t output_index) const
{
    if (output_index < m_denoised_outputs.size())
        return m_denoised_outputs[output_index].resource;
    return nullptr;
}

ID3D12Resource* PathTracer::albedo_resource(uint32_t output_index) const
{
    if (output_index < m_albedo_outputs.size())
        return m_albedo_outputs[output_index].resource;
    return nullptr;
}

ID3D12Resource* PathTracer::specular_albedo_resource(uint32_t output_index) const
{
    if (output_index < m_specular_albedo_outputs.size())
        return m_specular_albedo_outputs[output_index].resource;
    return nullptr;
}

ID3D12Resource* PathTracer::normals_aov_resource(uint32_t output_index) const
{
    if (output_index < m_normals_aov_outputs.size())
        return m_normals_aov_outputs[output_index].resource;
    return nullptr;
}

ID3D12Resource* PathTracer::roughness_aov_resource(uint32_t output_index) const
{
    if (output_index < m_roughness_aov_outputs.size())
        return m_roughness_aov_outputs[output_index].resource;
    return nullptr;
}

ID3D12Resource* PathTracer::gi_reservoir_resource(uint32_t output_index) const
{
    if (output_index < m_gi_reservoir_buffers.size())
        return m_gi_reservoir_buffers[output_index].resource;
    return nullptr;
}

} // namespace mars
