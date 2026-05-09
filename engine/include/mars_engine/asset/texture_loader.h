// =============================================================================
// texture_loader.h
// MARS 3D Engine — DirectXTex-based texture loader
//
// Loads DDS (BC7/BC6H/etc.), PNG, and EXR textures from disk into GPU
// DEFAULT-heap texture resources and registers them in the bindless heap.
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../d3d12_agility.h"

#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <optional>

// Forward-declare D3D12MA types to avoid leaking the D3D12MemAlloc header.
namespace D3D12MA { class Allocator; class Allocation; }

namespace mars
{

using Microsoft::WRL::ComPtr;

class DeviceContext;

// =============================================================================
// GpuTexture — a single GPU texture and its bindless slot
// =============================================================================
struct GpuTexture
{
    D3D12MA::Allocation* allocation = nullptr;
    ID3D12Resource*      resource   = nullptr;
    uint32_t             srv_slot   = UINT32_MAX;
    uint32_t             width      = 0;
    uint32_t             height     = 0;
    uint32_t             mip_levels = 0;
    DXGI_FORMAT          format     = DXGI_FORMAT_UNKNOWN;

    bool is_valid() const { return resource != nullptr; }

    void destroy();
};

// =============================================================================
// TextureLoader
// =============================================================================
class MARS_ENGINE_API TextureLoader
{
public:
    TextureLoader()  = default;
    ~TextureLoader() = default;

    TextureLoader(const TextureLoader&)            = delete;
    TextureLoader& operator=(const TextureLoader&) = delete;

    // Load a texture from disk (DDS, PNG, EXR, HDR, TGA, BMP, …).
    // Uploads to GPU DEFAULT heap and registers an SRV in the bindless heap.
    // `is_srgb` controls whether the view uses an _SRGB format variant.
    // Returns an invalid GpuTexture on failure (logs error).
    GpuTexture load(DeviceContext&      ctx,
                    D3D12MA::Allocator* allocator,
                    const std::string&  file_path,
                    bool                is_srgb = true) const;

    // Create a 1×1 solid-colour fallback texture (useful for missing textures).
    GpuTexture create_solid_color(DeviceContext&      ctx,
                                  D3D12MA::Allocator* allocator,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t a) const;
};

} // namespace mars
