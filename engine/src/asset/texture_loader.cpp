// =============================================================================
// texture_loader.cpp
// MARS 3D Engine — DirectXTex texture loader implementation
// =============================================================================

#include "mars_engine/asset/texture_loader.h"
#include "mars_engine/renderer/device_context.h"

#pragma warning(push, 0)
#include <D3D12MemAlloc.h>
#pragma warning(pop)

#include <DirectXTex.h>

#include <stdexcept>
#include <format>
#include <print>
#include <filesystem>
#include <cstring>

namespace mars
{

// ---------------------------------------------------------------------------
// GpuTexture::destroy
// ---------------------------------------------------------------------------
void GpuTexture::destroy()
{
    if (resource)   { resource->Release();    resource   = nullptr; }
    if (allocation) { allocation->Release();  allocation = nullptr; }
    srv_slot = UINT32_MAX;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void throw_if_failed(HRESULT hr, const char* msg)
{
    if (FAILED(hr))
        throw std::runtime_error(std::format("{} (HRESULT 0x{:08X})", msg, static_cast<unsigned>(hr)));
}

// Convert a narrow string path to wide for DirectXTex.
static std::wstring to_wide(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// Return an _SRGB variant of a DXGI_FORMAT if available.
static DXGI_FORMAT to_srgb(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_BC1_UNORM:             return DXGI_FORMAT_BC1_UNORM_SRGB;
    case DXGI_FORMAT_BC2_UNORM:             return DXGI_FORMAT_BC2_UNORM_SRGB;
    case DXGI_FORMAT_BC3_UNORM:             return DXGI_FORMAT_BC3_UNORM_SRGB;
    case DXGI_FORMAT_BC7_UNORM:             return DXGI_FORMAT_BC7_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM:        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8X8_UNORM:        return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    default:                                return fmt;
    }
}

// Upload a DirectXTex ScratchImage to a DEFAULT-heap resource using D3D12MA and the copy queue.
static GpuTexture upload_scratch(
    DeviceContext&                   ctx,
    D3D12MA::Allocator*              allocator,
    const DirectX::ScratchImage&     image,
    const DirectX::TexMetadata&      meta,
    DXGI_FORMAT                      view_format)
{
    GpuTexture tex;
    tex.width      = static_cast<uint32_t>(meta.width);
    tex.height     = static_cast<uint32_t>(meta.height);
    tex.mip_levels = static_cast<uint32_t>(meta.mipLevels);
    tex.format     = view_format;

    // ---- Create DEFAULT-heap texture resource ----
    D3D12_RESOURCE_DESC res_desc = {};
    res_desc.Dimension          = (meta.dimension == DirectX::TEX_DIMENSION_TEXTURE3D)
                                    ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                    : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    res_desc.Width              = meta.width;
    res_desc.Height             = static_cast<UINT>(meta.height);
    res_desc.DepthOrArraySize   = (meta.dimension == DirectX::TEX_DIMENSION_TEXTURE3D)
                                    ? static_cast<UINT16>(meta.depth)
                                    : static_cast<UINT16>(meta.arraySize);
    res_desc.MipLevels          = static_cast<UINT16>(meta.mipLevels);
    res_desc.Format             = meta.format;     // store in native format; view uses view_format
    res_desc.SampleDesc         = {1, 0};
    res_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    res_desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    D3D12MA::ALLOCATION_DESC alloc_desc{};
    alloc_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    throw_if_failed(
        allocator->CreateResource(
            &alloc_desc,
            &res_desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            &tex.allocation,
            IID_PPV_ARGS(&tex.resource)),
        "D3D12MA: CreateResource (texture) failed");

    tex.resource->SetName(L"GpuTexture");

    // ---- Determine required upload buffer size ----
    const uint32_t num_subresources = static_cast<uint32_t>(
        meta.mipLevels * meta.arraySize);

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(num_subresources);
    std::vector<UINT>                               row_counts(num_subresources);
    std::vector<UINT64>                             row_sizes(num_subresources);
    UINT64 total_bytes = 0;

    ctx.device()->GetCopyableFootprints(
        &res_desc, 0, num_subresources, 0,
        footprints.data(), row_counts.data(), row_sizes.data(), &total_bytes);

    // ---- Create upload buffer ----
    D3D12MA::ALLOCATION_DESC upload_alloc_desc{};
    upload_alloc_desc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC upload_buf_desc = CD3DX12_RESOURCE_DESC::Buffer(total_bytes);

    D3D12MA::Allocation* upload_alloc = nullptr;
    ID3D12Resource*      upload_buf   = nullptr;

    throw_if_failed(
        allocator->CreateResource(
            &upload_alloc_desc,
            &upload_buf_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            &upload_alloc,
            IID_PPV_ARGS(&upload_buf)),
        "D3D12MA: CreateResource (texture upload buffer) failed");

    // ---- Map and copy all subresources ----
    uint8_t* upload_data = nullptr;
    throw_if_failed(upload_buf->Map(0, nullptr, reinterpret_cast<void**>(&upload_data)),
        "Upload buffer Map failed");

    for (uint32_t sub = 0; sub < num_subresources; ++sub)
    {
        const DirectX::Image* img = image.GetImages();
        if (sub >= image.GetImageCount()) break;
        const DirectX::Image& src_img = img[sub];

        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = footprints[sub];
        uint8_t* dest_slice = upload_data + fp.Offset;

        for (uint32_t row = 0; row < row_counts[sub]; ++row)
        {
            std::memcpy(
                dest_slice + static_cast<size_t>(fp.Footprint.RowPitch) * row,
                src_img.pixels + src_img.rowPitch * row,
                static_cast<size_t>(row_sizes[sub]));
        }
    }
    upload_buf->Unmap(0, nullptr);

    // ---- Copy via copy queue ----
    ComPtr<ID3D12CommandAllocator>    cmd_alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd_list;
    throw_if_failed(
        ctx.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&cmd_alloc)),
        "CreateCommandAllocator (tex copy) failed");
    throw_if_failed(
        ctx.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
            cmd_alloc.Get(), nullptr, IID_PPV_ARGS(&cmd_list)),
        "CreateCommandList (tex copy) failed");

    for (uint32_t sub = 0; sub < num_subresources; ++sub)
    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = upload_buf;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint  = footprints[sub];

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = tex.resource;
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = sub;

        cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    throw_if_failed(cmd_list->Close(), "Copy command list Close (texture) failed");
    ID3D12CommandList* lists[] = {cmd_list.Get()};
    ctx.copy_queue()->ExecuteCommandLists(1, lists);

    // Wait for copy to finish.
    ComPtr<ID3D12Fence> fence;
    HANDLE              evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    throw_if_failed(ctx.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
        "CreateFence (tex upload) failed");
    throw_if_failed(ctx.copy_queue()->Signal(fence.Get(), 1), "Signal (tex upload) failed");
    if (fence->GetCompletedValue() < 1)
    {
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObjectEx(evt, INFINITE, FALSE);
    }
    CloseHandle(evt);

    upload_buf->Release();
    upload_alloc->Release();

    // ---- Register SRV in bindless heap ----
    tex.srv_slot = ctx.allocate_bindless_slot();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format                  = view_format;
    srv_desc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels     = static_cast<UINT>(meta.mipLevels);

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle =
        ctx.bindless_heap()->GetCPUDescriptorHandleForHeapStart();
    cpu_handle.ptr += static_cast<SIZE_T>(tex.srv_slot) * ctx.bindless_descriptor_size();

    ctx.device()->CreateShaderResourceView(tex.resource, &srv_desc, cpu_handle);

    return tex;
}

// ---------------------------------------------------------------------------
// TextureLoader::load
// ---------------------------------------------------------------------------
GpuTexture TextureLoader::load(
    DeviceContext&      ctx,
    D3D12MA::Allocator* allocator,
    const std::string&  file_path,
    bool                is_srgb) const
{
    namespace fs = std::filesystem;

    if (!fs::exists(file_path))
    {
        std::println(stderr, "[TextureLoader] File not found: {}", file_path);
        return {};
    }

    std::wstring wpath = to_wide(file_path);
    const std::string ext = fs::path(file_path).extension().string();

    DirectX::ScratchImage image;
    DirectX::TexMetadata  meta;

    HRESULT hr = E_FAIL;
    if (ext == ".dds" || ext == ".DDS")
    {
        hr = DirectX::LoadFromDDSFile(wpath.c_str(), DirectX::DDS_FLAGS_NONE, &meta, image);
    }
    else if (ext == ".hdr" || ext == ".HDR" || ext == ".exr" || ext == ".EXR")
    {
        hr = DirectX::LoadFromHDRFile(wpath.c_str(), &meta, image);
    }
    else
    {
        // PNG, TGA, BMP, JPEG, etc.
        hr = DirectX::LoadFromWICFile(wpath.c_str(), DirectX::WIC_FLAGS_NONE, &meta, image);
    }

    if (FAILED(hr))
    {
        std::println(stderr, "[TextureLoader] Failed to load '{}' (HRESULT 0x{:08X})", file_path, static_cast<unsigned>(hr));
        return {};
    }

    // Generate mips if only one level (skip for block-compressed formats).
    const bool is_compressed = DirectX::IsCompressed(meta.format);
    if (!is_compressed && meta.mipLevels == 1)
    {
        DirectX::ScratchImage mipped;
        hr = DirectX::GenerateMipMaps(*image.GetImages(), DirectX::TEX_FILTER_DEFAULT, 0, mipped);
        if (SUCCEEDED(hr))
        {
            image  = std::move(mipped);
            meta   = image.GetMetadata();
        }
    }

    DXGI_FORMAT view_format = is_srgb ? to_srgb(meta.format) : meta.format;

    try
    {
        GpuTexture tex = upload_scratch(ctx, allocator, image, meta, view_format);
        std::println("[TextureLoader] Loaded '{}' ({}×{}, {} mips, slot {})",
            file_path, tex.width, tex.height, tex.mip_levels, tex.srv_slot);
        return tex;
    }
    catch (const std::exception& e)
    {
        std::println(stderr, "[TextureLoader] Upload failed for '{}': {}", file_path, e.what());
        return {};
    }
}

// ---------------------------------------------------------------------------
// TextureLoader::create_solid_color
// ---------------------------------------------------------------------------
GpuTexture TextureLoader::create_solid_color(
    DeviceContext&      ctx,
    D3D12MA::Allocator* allocator,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a) const
{
    DirectX::ScratchImage image;
    DirectX::TexMetadata  meta{};
    meta.width      = 1;
    meta.height     = 1;
    meta.depth      = 1;
    meta.arraySize  = 1;
    meta.mipLevels  = 1;
    meta.format     = DXGI_FORMAT_R8G8B8A8_UNORM;
    meta.dimension  = DirectX::TEX_DIMENSION_TEXTURE2D;

    HRESULT hr = image.Initialize(meta);
    if (FAILED(hr)) return {};

    uint8_t pixel[4] = {r, g, b, a};
    std::memcpy(image.GetPixels(), pixel, 4);

    return upload_scratch(ctx, allocator, image, meta, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
}

} // namespace mars
