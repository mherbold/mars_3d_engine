// =============================================================================
// path_tracer.cpp
// MARS 3D Engine — DXR path-tracing pipeline implementation
// =============================================================================

#include "mars_engine/renderer/path_tracer.h"
#include "mars_engine/renderer/device_context.h"

#include <stdexcept>
#include <format>
#include <fstream>
#include <vector>
#include <string>
#include <cassert>
#include <print>

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

    std::println("[PathTracer] Searching for DXIL blob:");
    std::println("[PathTracer]   Try 1: '{}'", wstr_to_str(path1));

    auto blob = try_load(path1);
    if (blob.empty())
    {
        std::println("[PathTracer]   Try 1 FAILED — not found or empty.");
        std::println("[PathTracer]   Try 2: '{}'", wstr_to_str(path2));
        blob = try_load(path2);
        if (blob.empty())
            std::println("[PathTracer]   Try 2 FAILED — not found or empty.");
        else
            std::println("[PathTracer]   Try 2 OK — {} bytes loaded.", blob.size());
    }
    else
    {
        std::println("[PathTracer]   Try 1 OK — {} bytes loaded.", blob.size());
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
        if (b.alloc) { b.alloc->Release(); b.alloc = nullptr; b.resource = nullptr; }
    m_blas_list.clear();

    m_rtpso.Reset();
    m_rtpso_props.Reset();
    m_global_root_sig.Reset();
    m_local_root_sig.Reset();

    m_blit_pso_sdr.Reset();
    m_blit_pso_hdr10.Reset();
    m_blit_pso_scrgb.Reset();
    m_blit_root_sig.Reset();

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
        static constexpr UINT k_unbounded = UINT_MAX;
        static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS k_volatile_flags =
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
            D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

        // All register-space arrays in HLSL are backed by the same physical bindless
        // heap starting at descriptor index 0.  Every range must therefore specify
        // offsetInDescriptorsFromTableStart = 0 explicitly; using the default
        // D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND after an unbounded (UINT_MAX) range
        // causes an arithmetic overflow and E_INVALIDARG during serialization.
        CD3DX12_DESCRIPTOR_RANGE1 ranges[9]{};
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

        params[1].InitAsDescriptorTable(9, ranges, D3D12_SHADER_VISIBILITY_ALL);

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
    lib->DefineExport(L"Miss");
    lib->DefineExport(L"ShadowMiss");

    // Hit group subobject (primary)
    auto* hg = so_desc.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hg->SetClosestHitShaderImport(L"ClosestHit");
    hg->SetHitGroupExport(L"HitGroup_Primary");
    hg->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

    // Shader config: payload = float3 radiance + float hit_t + bool missed
    auto* shader_cfg = so_desc.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    shader_cfg->Config(
        sizeof(float) * 4 + sizeof(uint32_t), // payload size (radiance xyz, hit_t, missed)
        sizeof(float) * 2);                   // attribute size (barycentrics)

    // Pipeline config: max recursion depth 2 (primary + shadow)
    auto* pipe_cfg = so_desc.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    pipe_cfg->Config(2);

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
    lrs_assoc->AddExport(L"Miss");
    lrs_assoc->AddExport(L"ShadowMiss");
    lrs_assoc->AddExport(L"HitGroup_Primary");

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

    // Hit group table: 1 record (HitGroup_Primary)
    uint64_t hitgroup_size = align_up(k_hitgroup_record_stride,
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
// ---------------------------------------------------------------------------
void PathTracer::resize_output(DeviceContext& ctx,
                                uint32_t output_index,
                                uint32_t new_width, uint32_t new_height)
{
    assert(output_index < m_output_count);
    OutputTexture& out = m_outputs[output_index];

    if (out.width == new_width && out.height == new_height) return;

    // Release old texture
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
    // DLSS-RR reads from m_outputs (noisy) and writes to m_denoised_outputs.
    {
        OutputTexture& dn = m_denoised_outputs[output_index];
        if (dn.alloc) { dn.alloc->Release(); dn.alloc = nullptr; dn.resource = nullptr; }
        dn.width  = new_width;
        dn.height = new_height;

        D3D12_RESOURCE_DESC dn_desc = tex_desc; // RGBA16F, UAV flag

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
}

// ---------------------------------------------------------------------------
// build_blas
// ---------------------------------------------------------------------------
uint32_t PathTracer::build_blas(DeviceContext& ctx,
                                 const GpuMeshBuffer& mesh,
                                 bool allow_update)
{
    auto* device = ctx.device();

    D3D12_RAYTRACING_GEOMETRY_DESC geom_desc{};
    geom_desc.Type                                 = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom_desc.Flags                                = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
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

    // Allocate scratch buffer (DEFAULT heap, UAV)
    D3D12MA::Allocation* scratch_alloc = nullptr;
    ID3D12Resource*      scratch       = nullptr;
    {
        D3D12MA::ALLOCATION_DESC ad{};  ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = prebuild.ScratchDataSizeInBytes;
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
        bd.Width            = prebuild.ResultDataMaxSizeInBytes;
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

    // Free scratch immediately after build
    scratch_alloc->Release();

    uint32_t index = static_cast<uint32_t>(m_blas_list.size());
    m_blas_list.push_back({ result_alloc, result });
    return index;
}

// ---------------------------------------------------------------------------
// set_instance
// ---------------------------------------------------------------------------
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
                             bool allow_update)
{
    auto* device = ctx.device();

    uint32_t instance_count = static_cast<uint32_t>(m_instances.size());
    if (instance_count == 0)
    {
        std::println("[PathTracer] build_tlas: instance_count == 0 — TLAS not built, tlas_srv_slot stays UINT32_MAX.");
        return;
    }
    std::println("[PathTracer] build_tlas: building TLAS for {} instance(s).", instance_count);

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
        d.InstanceContributionToHitGroupIndex = 0;
        d.Flags                               = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        d.AccelerationStructure               = m_blas_list[inst.blas_index].resource->GetGPUVirtualAddress();
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

    std::println("[PathTracer] build_tlas: TLAS built successfully. tlas_srv_slot = {}.", m_tlas_srv_slot);

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

    std::println("[PathTracer] upload_scene_buffers: instance_srv={} material_srv={}",
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
    fc.sun_direction            = sun_direction;
    fc.sun_intensity            = sun_intensity;
    fc.sun_color                = sun_color;
    fc.prev_view_proj           = prev_view_proj;

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
            std::println("[PathTracer] trace() early-return: initialised={} tlas_srv_slot={}",
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
            std::println("[PathTracer] trace() early-return: output[{}] not ready ({}x{}, resource={})",
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
    rays.HitGroupTable.SizeInBytes   = k_hitgroup_record_stride;
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

} // namespace mars
