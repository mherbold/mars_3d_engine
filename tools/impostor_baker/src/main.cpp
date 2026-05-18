// =============================================================================
// mars_impostor_baker - Offline impostor atlas baking tool
//
// Renders N octahedral views of a vegetation species mesh into a texture atlas
// using D3D12 rasterization and saves the result as a DDS file.
//
// Usage:
//   mars_impostor_baker --model <path/to/model.fbx>
//                       --output <path/to/atlas.dds>
//                      [--views <N>]          default: 16
//                      [--size <pixels>]       default: 2048
// =============================================================================

#include <mars_engine/mars_engine.h>

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXTex.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>
#include <filesystem>
#include <fstream>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static void print_usage()
{
    std::printf("Usage:\n  mars_impostor_baker --model <mesh.fbx> --output <atlas.dds> [--views N] [--size px]\n");
    std::printf("                      [--debug-colors]  (generate a solid-color-per-cell debug atlas, no model required)\n");
}

static void fatal(const char* msg)
{
    std::fprintf(stderr, "[impostor_baker] FATAL: %s\n", msg);
    std::exit(1);
}

static std::vector<uint8_t> load_dxil(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open())
        throw std::runtime_error("Cannot open DXIL: " + path);
    const auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> data(sz);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sz));
    return data;
}

static mars::Vec3 octahedral_direction(int ix, int iy, int N)
{
    float u = (static_cast<float>(ix) + 0.5f) / static_cast<float>(N) * 2.0f - 1.0f;
    float v = (static_cast<float>(iy) + 0.5f) / static_cast<float>(N) * 2.0f - 1.0f;
    float y = 1.0f - (std::abs(u) + std::abs(v));
    if (y < 0.0f) { float old_u = u; u = (1.0f - std::abs(v)) * (u >= 0.0f ? 1.0f : -1.0f); v = (1.0f - std::abs(old_u)) * (v >= 0.0f ? 1.0f : -1.0f); }
    mars::Vec3 dir{u, y, v};
    float len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (len > 1e-6f) { dir.x /= len; dir.y /= len; dir.z /= len; }
    return dir;
}

static void look_at_matrix(const mars::Vec3& eye, const mars::Vec3& target, const mars::Vec3& up_hint, float out[16])
{
    // Left-handed look-at (+Z toward target).
    // HLSL: mul(row_vec, M) — basis vectors must live in the COLUMNS of the HLSL float4x4.
    // With row-major packing M[row][col] = out[row*4+col], so column j = out[j], out[4+j], out[8+j], out[12+j].
    // Layout in memory (out[row*4+col]):
    //   col 0 = right (r),  col 1 = up (u),  col 2 = forward (f),  col 3 = (0,0,0,1)
    //   row 3 = translation: -dot(r,e), -dot(u,e), -dot(f,e), 1
    mars::Vec3 f{target.x-eye.x, target.y-eye.y, target.z-eye.z};
    float flen = std::sqrt(f.x*f.x+f.y*f.y+f.z*f.z);
    if (flen > 1e-6f) { f.x/=flen; f.y/=flen; f.z/=flen; }
    mars::Vec3 r{up_hint.y*f.z-up_hint.z*f.y, up_hint.z*f.x-up_hint.x*f.z, up_hint.x*f.y-up_hint.y*f.x};
    float rlen = std::sqrt(r.x*r.x+r.y*r.y+r.z*r.z);
    if (rlen > 1e-6f) { r.x/=rlen; r.y/=rlen; r.z/=rlen; }
    mars::Vec3 u{f.y*r.z-f.z*r.y, f.z*r.x-f.x*r.z, f.x*r.y-f.y*r.x};
    // col0=r, col1=u, col2=f, col3=(0,0,0,1); translation in row 3
    out[0]=r.x; out[1]=u.x; out[2]=f.x; out[3]=0.0f;
    out[4]=r.y; out[5]=u.y; out[6]=f.y; out[7]=0.0f;
    out[8]=r.z; out[9]=u.z; out[10]=f.z; out[11]=0.0f;
    out[12]=-(r.x*eye.x+r.y*eye.y+r.z*eye.z);
    out[13]=-(u.x*eye.x+u.y*eye.y+u.z*eye.z);
    out[14]=-(f.x*eye.x+f.y*eye.y+f.z*eye.z);
    out[15]=1.0f;
}

static void ortho_matrix(float l, float r, float b, float t, float zn, float zf, float out[16])
{
    // Left-handed D3D ortho for mul(row_vec, M): basis vectors in columns.
    // out[row*4+col]; translation in row 3.
    float rl=r-l, tb=t-b, fn=zf-zn;
    out[0]=2.0f/rl; out[1]=0.0f;    out[2]=0.0f;     out[3]=0.0f;
    out[4]=0.0f;    out[5]=2.0f/tb; out[6]=0.0f;     out[7]=0.0f;
    out[8]=0.0f;    out[9]=0.0f;    out[10]=1.0f/fn; out[11]=0.0f;
    out[12]=-(r+l)/rl; out[13]=-(t+b)/tb; out[14]=-zn/fn; out[15]=1.0f;
}

static void mat_mul(const float a[16], const float b[16], float out[16])
{
    for (int row=0;row<4;++row) for (int col=0;col<4;++col) { out[row*4+col]=0.0f; for (int k=0;k<4;++k) out[row*4+col]+=a[row*4+k]*b[k*4+col]; }
}

struct ImpostorBaker
{
    mars::DeviceContext   device_ctx;
    mars::ResourceManager resource_mgr;

    ComPtr<ID3D12Resource>       rt_resource;
    D3D12_CPU_DESCRIPTOR_HANDLE  rt_rtv{};
    ComPtr<ID3D12Resource>       rt_dn_resource;   // depth-normal RT (RT1)
    D3D12_CPU_DESCRIPTOR_HANDLE  rt_dn_rtv{};
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12Resource>       depth_resource;
    D3D12_CPU_DESCRIPTOR_HANDLE  dsv{};
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    ComPtr<ID3D12Resource>       cell_readback;
    ComPtr<ID3D12Resource>       cell_dn_readback; // readback for RT1
    uint64_t                     cell_row_pitch = 0;
    ComPtr<ID3D12Fence>          fence;
    HANDLE                       fence_event = nullptr;
    uint64_t                     fence_value = 0;
    ComPtr<ID3D12CommandAllocator>    cmd_alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd_list;
    ComPtr<ID3D12RootSignature>  root_sig;
    ComPtr<ID3D12PipelineState>  pso;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    uint32_t                     srv_descriptor_size = 0;
    uint32_t atlas_size = 2048;
    uint32_t view_count = 16;
    uint32_t cell_size  = 128;

    void init(uint32_t size, uint32_t views)
    {
        atlas_size = size; view_count = views; cell_size = atlas_size / view_count;
        device_ctx.init();
        resource_mgr.init(device_ctx);
        create_cell_render_targets();
        create_command_infrastructure();
        create_srv_heap();
        load_and_create_pipeline();
        std::printf("[impostor_baker] D3D12 ready. Atlas %ux%u, %ux%u views, cell %upx.\n", atlas_size, atlas_size, view_count, view_count, cell_size);
    }

    void create_cell_render_targets()
    {
        auto* device = device_ctx.device();
        const uint32_t rtv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{}; rtv_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtv_desc.NumDescriptors=2;
        device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap));
        rt_rtv    = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rt_dn_rtv = rt_rtv; rt_dn_rtv.ptr += rtv_inc;
        D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{}; dsv_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV; dsv_desc.NumDescriptors=1;
        device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap));
        dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RESOURCE_DESC rt_tex{};
        rt_tex.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; rt_tex.Width=cell_size; rt_tex.Height=cell_size;
        rt_tex.DepthOrArraySize=1; rt_tex.MipLevels=1; rt_tex.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        rt_tex.SampleDesc={1,0}; rt_tex.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{}; cv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        CD3DX12_HEAP_PROPERTIES dh(D3D12_HEAP_TYPE_DEFAULT);
        // RT0 — base color
        device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &rt_tex, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt_resource));
        rt_resource->SetName(L"ImpostorBaker::CellRT");
        device->CreateRenderTargetView(rt_resource.Get(), nullptr, rt_rtv);
        // RT1 — depth/normal (same format: RGBA8)
        device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &rt_tex, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt_dn_resource));
        rt_dn_resource->SetName(L"ImpostorBaker::CellDNRT");
        device->CreateRenderTargetView(rt_dn_resource.Get(), nullptr, rt_dn_rtv);
        // Depth buffer
        D3D12_RESOURCE_DESC depth_tex=rt_tex; depth_tex.Format=DXGI_FORMAT_D32_FLOAT; depth_tex.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE dcv{}; dcv.Format=DXGI_FORMAT_D32_FLOAT; dcv.DepthStencil.Depth=1.0f;
        device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &depth_tex, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv, IID_PPV_ARGS(&depth_resource));
        depth_resource->SetName(L"ImpostorBaker::CellDepth");
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvd{}; dsvd.Format=DXGI_FORMAT_D32_FLOAT; dsvd.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(depth_resource.Get(), &dsvd, dsv);
        // Readback buffers
        cell_row_pitch = ((static_cast<uint64_t>(cell_size)*4u+255u)&~255u);
        D3D12_RESOURCE_DESC rb_desc=CD3DX12_RESOURCE_DESC::Buffer(cell_row_pitch*cell_size);
        CD3DX12_HEAP_PROPERTIES rh(D3D12_HEAP_TYPE_READBACK);
        device->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&cell_readback));
        cell_readback->SetName(L"ImpostorBaker::CellReadback");
        device->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&cell_dn_readback));
        cell_dn_readback->SetName(L"ImpostorBaker::CellDNReadback");
    }

    void create_command_infrastructure()
    {
        auto* device = device_ctx.device();
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_alloc));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_alloc.Get(), nullptr, IID_PPV_ARGS(&cmd_list));
        cmd_list->Close();
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    }

    void create_srv_heap()
    {
        auto* device = device_ctx.device();
        D3D12_DESCRIPTOR_HEAP_DESC desc{}; desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; desc.NumDescriptors=256; desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&srv_heap));
        srv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void load_and_create_pipeline()
    {
        auto* device = device_ctx.device();
        auto vs_dxil = load_dxil("dxil/impostor_bake_vs.dxil");
        auto ps_dxil = load_dxil("dxil/impostor_bake_ps.dxil");
        HRESULT hr = device->CreateRootSignature(0, vs_dxil.data(), vs_dxil.size(), IID_PPV_ARGS(&root_sig));
        if (FAILED(hr)) throw std::runtime_error("Failed to create root signature.");
        D3D12_INPUT_ELEMENT_DESC il[] = {
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0, 0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"NORMAL",  0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TANGENT", 0,DXGI_FORMAT_R32G32B32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,   0,36,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature        = root_sig.Get();
        pso_desc.VS                    = {vs_dxil.data(), vs_dxil.size()};
        pso_desc.PS                    = {ps_dxil.data(), ps_dxil.size()};
        pso_desc.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso_desc.SampleMask            = UINT_MAX;
        pso_desc.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso_desc.InputLayout           = {il, _countof(il)};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets      = 2;
        pso_desc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.RTVFormats[1]         = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
        pso_desc.SampleDesc            = {1,0};
        hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) throw std::runtime_error("Failed to create impostor baker PSO.");
        std::printf("[impostor_baker] Rasterization pipeline ready.\n");
    }

    void flush()
    {
        ++fence_value;
        device_ctx.direct_queue()->Signal(fence.Get(), fence_value);
        if (fence->GetCompletedValue() < fence_value) { fence->SetEventOnCompletion(fence_value, fence_event); WaitForSingleObject(fence_event, INFINITE); }
    }

    bool upload_material_texture(uint32_t slot, const std::string& tex_path, ID3D12Resource** out_resource)
    {
        auto* device = device_ctx.device();
        DirectX::ScratchImage scratch;
        HRESULT hr;
        std::wstring wide(tex_path.begin(), tex_path.end());
        if (tex_path.ends_with(".dds")||tex_path.ends_with(".DDS")) hr=DirectX::LoadFromDDSFile(wide.c_str(),DirectX::DDS_FLAGS_NONE,nullptr,scratch);
        else if (tex_path.ends_with(".hdr")||tex_path.ends_with(".HDR")) hr=DirectX::LoadFromHDRFile(wide.c_str(),nullptr,scratch);
        else hr=DirectX::LoadFromWICFile(wide.c_str(),DirectX::WIC_FLAGS_NONE,nullptr,scratch);
        if (FAILED(hr)) { std::printf("[impostor_baker] WARNING: Cannot load texture: %s\n",tex_path.c_str()); return false; }
        ComPtr<ID3D12Resource> tex;
        std::vector<D3D12_SUBRESOURCE_DATA> subresources;
        if (FAILED(DirectX::CreateTexture(device, scratch.GetMetadata(), &tex))) return false;
        if (FAILED(DirectX::PrepareUpload(device,scratch.GetImages(),scratch.GetImageCount(),scratch.GetMetadata(),subresources))) return false;
        const UINT64 upload_size=GetRequiredIntermediateSize(tex.Get(),0,static_cast<UINT>(subresources.size()));
        CD3DX12_HEAP_PROPERTIES uh(D3D12_HEAP_TYPE_UPLOAD);
        ComPtr<ID3D12Resource> ubuf;
        auto ubuf_desc = CD3DX12_RESOURCE_DESC::Buffer(upload_size);
        device->CreateCommittedResource(&uh,D3D12_HEAP_FLAG_NONE,&ubuf_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&ubuf));
        cmd_alloc->Reset(); cmd_list->Reset(cmd_alloc.Get(),nullptr);
        auto b0=CD3DX12_RESOURCE_BARRIER::Transition(tex.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        cmd_list->ResourceBarrier(1,&b0);
        UpdateSubresources(cmd_list.Get(),tex.Get(),ubuf.Get(),0,0,static_cast<UINT>(subresources.size()),subresources.data());
        auto b1=CD3DX12_RESOURCE_BARRIER::Transition(tex.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd_list->ResourceBarrier(1,&b1); cmd_list->Close();
        ID3D12CommandList* lists[]={cmd_list.Get()}; device_ctx.direct_queue()->ExecuteCommandLists(1,lists); flush();
        D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu=srv_heap->GetCPUDescriptorHandleForHeapStart();
        srv_cpu.ptr+=static_cast<SIZE_T>(slot)*srv_descriptor_size;
        device->CreateShaderResourceView(tex.Get(),nullptr,srv_cpu);
        *out_resource=tex.Detach(); return true;
    }

    void create_white_texture(uint32_t slot, ID3D12Resource** out_resource)
    {
        auto* device = device_ctx.device();
        D3D12_RESOURCE_DESC td{}; td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width=1; td.Height=1;
        td.DepthOrArraySize=1; td.MipLevels=1; td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc={1,0};
        CD3DX12_HEAP_PROPERTIES dh(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> tex;
        device->CreateCommittedResource(&dh,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&tex));
        uint32_t white=0xFFFFFFFFu;
        CD3DX12_HEAP_PROPERTIES uh(D3D12_HEAP_TYPE_UPLOAD);
        ComPtr<ID3D12Resource> ubuf;
        auto ubuf_desc = CD3DX12_RESOURCE_DESC::Buffer(GetRequiredIntermediateSize(tex.Get(),0,1));
        device->CreateCommittedResource(&uh,D3D12_HEAP_FLAG_NONE,&ubuf_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&ubuf));
        D3D12_SUBRESOURCE_DATA sub{}; sub.pData=&white; sub.RowPitch=4; sub.SlicePitch=4;
        cmd_alloc->Reset(); cmd_list->Reset(cmd_alloc.Get(),nullptr);
        UpdateSubresources(cmd_list.Get(),tex.Get(),ubuf.Get(),0,0,1,&sub);
        auto b=CD3DX12_RESOURCE_BARRIER::Transition(tex.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd_list->ResourceBarrier(1,&b); cmd_list->Close();
        ID3D12CommandList* lists[]={cmd_list.Get()}; device_ctx.direct_queue()->ExecuteCommandLists(1,lists); flush();
        D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu=srv_heap->GetCPUDescriptorHandleForHeapStart();
        srv_cpu.ptr+=static_cast<SIZE_T>(slot)*srv_descriptor_size;
        device->CreateShaderResourceView(tex.Get(),nullptr,srv_cpu);
        *out_resource=tex.Detach();
    }

    void compute_ortho_mvp(const mars::Vec3& dir, const mars::AABB& bounds, float mvp[16])
    {
        mars::Vec3 centre{(bounds.min_pt.x+bounds.max_pt.x)*0.5f,(bounds.min_pt.y+bounds.max_pt.y)*0.5f,(bounds.min_pt.z+bounds.max_pt.z)*0.5f};
        float dx=(bounds.max_pt.x-bounds.min_pt.x)*0.5f, dy=(bounds.max_pt.y-bounds.min_pt.y)*0.5f, dz=(bounds.max_pt.z-bounds.min_pt.z)*0.5f;
        float radius=std::sqrt(dx*dx+dy*dy+dz*dz);
        float dist=radius*2.5f;
        mars::Vec3 eye{centre.x+dir.x*dist, centre.y+dir.y*dist, centre.z+dir.z*dist};
        mars::Vec3 up{0.0f,1.0f,0.0f}; if (std::abs(dir.y)>0.99f) up={0.0f,0.0f,1.0f};
        float view[16], proj[16];
        look_at_matrix(eye,centre,up,view);
        ortho_matrix(-radius,radius,-radius,radius,0.1f,dist+radius*2.0f,proj);
        mat_mul(view,proj,mvp);
    }

    void render_cell(const mars::GpuModel& model, const std::vector<ID3D12Resource*>& mat_textures, const mars::Vec3& dir, std::vector<uint8_t>& cell_rgba, std::vector<uint8_t>& cell_dn_rgba)
    {
        auto* queue=device_ctx.direct_queue();
        cell_rgba.resize(cell_size*cell_size*4,0);
        cell_dn_rgba.resize(cell_size*cell_size*4,0);
        cmd_alloc->Reset(); cmd_list->Reset(cmd_alloc.Get(),pso.Get());
        float cc[4]={0,0,0,0};
        cmd_list->ClearRenderTargetView(rt_rtv,cc,0,nullptr);
        cmd_list->ClearRenderTargetView(rt_dn_rtv,cc,0,nullptr);
        cmd_list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2]={rt_rtv,rt_dn_rtv};
        cmd_list->OMSetRenderTargets(2,rtvs,FALSE,&dsv);
        D3D12_VIEWPORT vp{0,0,static_cast<float>(cell_size),static_cast<float>(cell_size),0,1};
        D3D12_RECT sc{0,0,static_cast<LONG>(cell_size),static_cast<LONG>(cell_size)};
        cmd_list->RSSetViewports(1,&vp); cmd_list->RSSetScissorRects(1,&sc);
        cmd_list->SetGraphicsRootSignature(root_sig.Get());
        cmd_list->SetPipelineState(pso.Get());
        float mvp[16]; compute_ortho_mvp(dir,model.bounds,mvp);
        cmd_list->SetGraphicsRoot32BitConstants(0,16,mvp,0);
        ID3D12DescriptorHeap* heaps[]={srv_heap.Get()}; cmd_list->SetDescriptorHeaps(1,heaps);
        cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (uint32_t mi=0; mi<static_cast<uint32_t>(model.mesh_buffers.size()); ++mi)
        {
            const mars::GpuMeshBuffer& mb=model.mesh_buffers[mi];
            if (!mb.is_valid()) continue;
            if (mi == 0)
            {
                auto vbv0=mb.vertex_buffer_view();
                static bool printed_stride = false;
                if (!printed_stride) { std::printf("[impostor_baker]   mesh[0] vertex stride=%u, indices=%u\n", vbv0.StrideInBytes, mb.index_count()); printed_stride = true; }
            }
            uint32_t mat_idx=(mi<model.mesh_material_indices.size())?model.mesh_material_indices[mi]:0u;
            if (mat_idx>=static_cast<uint32_t>(mat_textures.size())) mat_idx=0u;
            D3D12_GPU_DESCRIPTOR_HANDLE sg=srv_heap->GetGPUDescriptorHandleForHeapStart();
            sg.ptr+=static_cast<UINT64>(mat_idx)*srv_descriptor_size;
            cmd_list->SetGraphicsRootDescriptorTable(1,sg);
            auto vbv=mb.vertex_buffer_view(); auto ibv=mb.index_buffer_view();
            cmd_list->IASetVertexBuffers(0,1,&vbv); cmd_list->IASetIndexBuffer(&ibv);
            cmd_list->DrawIndexedInstanced(mb.index_count(),1,0,0,0);
        }
        // Transition and copy RT0
        D3D12_RESOURCE_BARRIER barriers_to_copy[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(rt_resource.Get(),    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(rt_dn_resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE)
        };
        cmd_list->ResourceBarrier(2, barriers_to_copy);

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src_loc.SubresourceIndex=0;
        D3D12_TEXTURE_COPY_LOCATION dst_loc{}; dst_loc.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        dst_loc.PlacedFootprint.Footprint.Width=cell_size; dst_loc.PlacedFootprint.Footprint.Height=cell_size;
        dst_loc.PlacedFootprint.Footprint.Depth=1; dst_loc.PlacedFootprint.Footprint.RowPitch=static_cast<UINT>(cell_row_pitch);

        src_loc.pResource=rt_resource.Get(); dst_loc.pResource=cell_readback.Get();
        cmd_list->CopyTextureRegion(&dst_loc,0,0,0,&src_loc,nullptr);
        src_loc.pResource=rt_dn_resource.Get(); dst_loc.pResource=cell_dn_readback.Get();
        cmd_list->CopyTextureRegion(&dst_loc,0,0,0,&src_loc,nullptr);

        D3D12_RESOURCE_BARRIER barriers_to_rt[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(rt_resource.Get(),    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(rt_dn_resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)
        };
        cmd_list->ResourceBarrier(2, barriers_to_rt);
        cmd_list->Close();
        ID3D12CommandList* lists[]={cmd_list.Get()}; queue->ExecuteCommandLists(1,lists); flush();

        // Readback RT0
        void* mapped=nullptr;
        cell_readback->Map(0,nullptr,&mapped);
        const uint8_t* src=static_cast<const uint8_t*>(mapped);
        for (uint32_t row=0;row<cell_size;++row) std::memcpy(cell_rgba.data()+row*cell_size*4, src+row*cell_row_pitch, cell_size*4u);
        D3D12_RANGE ur{0,0}; cell_readback->Unmap(0,&ur);

        // Readback RT1
        cell_dn_readback->Map(0,nullptr,&mapped);
        src=static_cast<const uint8_t*>(mapped);
        for (uint32_t row=0;row<cell_size;++row) std::memcpy(cell_dn_rgba.data()+row*cell_size*4, src+row*cell_row_pitch, cell_size*4u);
        cell_dn_readback->Unmap(0,&ur);
    }

    std::vector<uint8_t> bake_debug_colors()
    {
        // Fill each cell with a unique HSV-derived solid colour so we can verify
        // at runtime that the atlas UV lookup is hitting the expected cell.
        std::vector<uint8_t> atlas(atlas_size * atlas_size * 4, 0);
        const uint32_t total = view_count * view_count;
        for (uint32_t iy = 0; iy < view_count; ++iy)
        {
            for (uint32_t ix = 0; ix < view_count; ++ix)
            {
                const uint32_t cell_idx = iy * view_count + ix;
                // Spread hue evenly across all cells; saturation/value fixed.
                const float hue = static_cast<float>(cell_idx) / static_cast<float>(total); // [0,1)
                const float s = 0.9f, v = 0.9f;
                // HSV → RGB
                const float h6 = hue * 6.0f;
                const int   hi = static_cast<int>(h6) % 6;
                const float f  = h6 - std::floor(h6);
                const float p  = v * (1.0f - s);
                const float q  = v * (1.0f - s * f);
                const float t  = v * (1.0f - s * (1.0f - f));
                float r, g, b;
                switch (hi)
                {
                    case 0: r=v; g=t; b=p; break;
                    case 1: r=q; g=v; b=p; break;
                    case 2: r=p; g=v; b=t; break;
                    case 3: r=p; g=q; b=v; break;
                    case 4: r=t; g=p; b=v; break;
                    default:r=v; g=p; b=q; break;
                }
                const uint8_t R = static_cast<uint8_t>(r * 255.0f + 0.5f);
                const uint8_t G = static_cast<uint8_t>(g * 255.0f + 0.5f);
                const uint8_t B = static_cast<uint8_t>(b * 255.0f + 0.5f);
                const uint8_t A = 255u;

                // Write the solid colour into the cell region, leaving a 2px white border
                // so cell boundaries are visible.
                const uint32_t cx = ix * cell_size;
                const uint32_t cy = iy * cell_size;
                for (uint32_t py = 0; py < cell_size; ++py)
                {
                    for (uint32_t px = 0; px < cell_size; ++px)
                    {
                        const bool border = (px < 2 || py < 2 ||
                                             px >= cell_size - 2 || py >= cell_size - 2);
                        const uint32_t out_idx = ((cy + py) * atlas_size + (cx + px)) * 4;
                        atlas[out_idx + 0] = border ? 255u : R;
                        atlas[out_idx + 1] = border ? 255u : G;
                        atlas[out_idx + 2] = border ? 255u : B;
                        atlas[out_idx + 3] = A;
                    }
                }
            }
        }
        std::printf("[impostor_baker] Debug-color atlas generated (%ux%u cells, %upx each).\n",
                    view_count, view_count, cell_size);
        return atlas;
    }

    void bake_views(const mars::GpuModel& model, const std::vector<ID3D12Resource*>& mat_textures,
                    std::vector<uint8_t>& atlas_out, std::vector<uint8_t>& dn_atlas_out)
    {
        std::printf("[impostor_baker] Model bounds: (%.3f,%.3f,%.3f) - (%.3f,%.3f,%.3f)\n",
            model.bounds.min_pt.x, model.bounds.min_pt.y, model.bounds.min_pt.z,
            model.bounds.max_pt.x, model.bounds.max_pt.y, model.bounds.max_pt.z);

        atlas_out.assign(atlas_size*atlas_size*4, 0);
        dn_atlas_out.assign(atlas_size*atlas_size*4, 0);
        std::vector<uint8_t> cell_rgba, cell_dn_rgba;
        uint64_t total_opaque_pixels = 0;
        std::printf("[impostor_baker] Baking %ux%u views (cell %upx) for model: %s\n", view_count, view_count, cell_size, model.name.c_str());
        for (uint32_t iy=0;iy<view_count;++iy)
        {
            for (uint32_t ix=0;ix<view_count;++ix)
            {
                mars::Vec3 dir=octahedral_direction(static_cast<int>(ix),static_cast<int>(iy),static_cast<int>(view_count));
                render_cell(model, mat_textures, dir, cell_rgba, cell_dn_rgba);
                uint32_t cx=ix*cell_size, cy=iy*cell_size;
                for (uint32_t py=0;py<cell_size;++py)
                {
                    std::memcpy(atlas_out.data()+((cy+py)*atlas_size+cx)*4, cell_rgba.data()+py*cell_size*4, cell_size*4u);
                    std::memcpy(dn_atlas_out.data()+((cy+py)*atlas_size+cx)*4, cell_dn_rgba.data()+py*cell_size*4, cell_size*4u);
                }
                // Count pixels with alpha > 0 in this cell to verify geometry rendered
                for (size_t p = 3; p < cell_rgba.size(); p += 4)
                    if (cell_rgba[p] > 0) ++total_opaque_pixels;
            }
            std::printf("[impostor_baker]   Row %u/%u done.\n", iy+1, view_count);
        }
        std::printf("[impostor_baker] Total non-transparent pixels baked: %llu / %llu\n",
            total_opaque_pixels,
            static_cast<uint64_t>(view_count) * view_count * cell_size * cell_size);
        if (total_opaque_pixels == 0)
            std::printf("[impostor_baker] WARNING: All pixels are transparent! Check model bounds, textures, and matrix setup.\n");
    }

    void save_dds(const std::vector<uint8_t>& rgba_pixels, const std::string& output_path)
    {
        DirectX::Image img{}; img.width=atlas_size; img.height=atlas_size;
        img.format=DXGI_FORMAT_R8G8B8A8_UNORM; img.rowPitch=atlas_size*4; img.slicePitch=atlas_size*atlas_size*4;
        img.pixels=const_cast<uint8_t*>(rgba_pixels.data());
        DirectX::ScratchImage compressed;
        HRESULT hr=DirectX::Compress(img,DXGI_FORMAT_BC3_UNORM,DirectX::TEX_COMPRESS_DEFAULT,DirectX::TEX_THRESHOLD_DEFAULT,compressed);
        std::wstring wpath(output_path.begin(),output_path.end());
        if (FAILED(hr)) { std::printf("[impostor_baker] WARNING: BC3 failed, saving uncompressed.\n"); hr=DirectX::SaveToDDSFile(img,DirectX::DDS_FLAGS_NONE,wpath.c_str()); }
        else hr=DirectX::SaveToDDSFile(compressed.GetImages(),compressed.GetImageCount(),compressed.GetMetadata(),DirectX::DDS_FLAGS_NONE,wpath.c_str());
        if (FAILED(hr)) fatal("Failed to write DDS.");
        std::printf("[impostor_baker] Saved: %s\n", output_path.c_str());
    }

    void shutdown()
    {
        flush();
        if (fence_event) CloseHandle(fence_event);
        resource_mgr.shutdown();
    }
};

int main(int argc, char** argv)
{
    std::string model_path, output_path;
    uint32_t view_count=16, atlas_size=2048;
    bool debug_colors = false;
    for (int i=1;i<argc;++i)
    {
        if      (std::strcmp(argv[i],"--model")==0 && i+1<argc)  model_path=argv[++i];
        else if (std::strcmp(argv[i],"--output")==0 && i+1<argc) output_path=argv[++i];
        else if (std::strcmp(argv[i],"--views")==0 && i+1<argc)  view_count=static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i],"--size")==0 && i+1<argc)   atlas_size=static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i],"--debug-colors")==0)        debug_colors = true;
        else if (std::strcmp(argv[i],"--help")==0)                { print_usage(); return 0; }
    }
    if (output_path.empty()) { print_usage(); return 1; }
    if (!debug_colors && model_path.empty()) { print_usage(); return 1; }
    if (!debug_colors && !fs::exists(model_path)) { std::fprintf(stderr,"[impostor_baker] Model not found: %s\n",model_path.c_str()); return 1; }
    fs::path od=fs::path(output_path).parent_path(); if (!od.empty()&&!fs::exists(od)) fs::create_directories(od);

    if (debug_colors)
    {
        std::printf("[impostor_baker] DEBUG-COLORS mode: output: %s  views: %ux%u  size: %ux%u px\n",
                    output_path.c_str(), view_count, view_count, atlas_size, atlas_size);
        try
        {
            ImpostorBaker baker;
            baker.init(atlas_size, view_count);
            auto atlas = baker.bake_debug_colors();
            baker.save_dds(atlas, output_path);
            baker.shutdown();
        }
        catch (const std::exception& ex) { std::fprintf(stderr,"[impostor_baker] ERROR: %s\n",ex.what()); return 1; }
        std::printf("[impostor_baker] Done.\n");
        return 0;
    }

    std::printf("[impostor_baker] model: %s\n[impostor_baker] output: %s\n[impostor_baker] views: %ux%u  size: %ux%u px\n", model_path.c_str(), output_path.c_str(), view_count, view_count, atlas_size, atlas_size);
    try
    {
        ImpostorBaker baker;
        baker.init(atlas_size, view_count);
        const uint32_t model_idx=baker.resource_mgr.load_model(baker.device_ctx, model_path, true);
        if (model_idx==UINT32_MAX) fatal("Failed to load model.");
        const mars::GpuModel& model=baker.resource_mgr.model(model_idx);
        if (model.mesh_buffers.empty()) fatal("No mesh buffers in model.");
        std::printf("[impostor_baker] Loaded: %s (%zu meshes, %zu materials)\n", model.name.c_str(), model.mesh_buffers.size(), model.materials.size());
        const uint32_t mat_count=static_cast<uint32_t>(model.materials.size());
        std::vector<ID3D12Resource*> mat_textures(mat_count,nullptr);
        for (uint32_t m=0;m<mat_count;++m)
        {
            const std::string& tp=model.materials[m].base_color_texture.path;
            if (!tp.empty()&&fs::exists(tp)) { if (!baker.upload_material_texture(m,tp,&mat_textures[m])) baker.create_white_texture(m,&mat_textures[m]); }
            else baker.create_white_texture(m,&mat_textures[m]);
        }
        std::vector<uint8_t> atlas, dn_atlas;
        baker.bake_views(model, mat_textures, atlas, dn_atlas);
        baker.save_dds(atlas, output_path);
        // Derive the depth-normal atlas path alongside the color atlas.
        std::string dn_output_path = fs::path(output_path).replace_filename("impostor_atlas_depth_normal.dds").string();
        baker.save_dds(dn_atlas, dn_output_path);
        for (auto* r:mat_textures) if (r) r->Release();
        baker.shutdown();
    }
    catch (const std::exception& ex) { std::fprintf(stderr,"[impostor_baker] ERROR: %s\n",ex.what()); return 1; }
    std::printf("[impostor_baker] Done.\n");
    return 0;
}
