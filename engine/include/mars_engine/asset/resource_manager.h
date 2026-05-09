// =============================================================================
// resource_manager.h
// MARS 3D Engine — GPU resource manager
//
// Owns the D3D12MA allocator, the AssetImporter, the TextureLoader, and all
// live GpuMeshBuffer / GpuTexture objects.  Provides load_model() and
// load_texture() as the primary entry points for the asset pipeline.
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "asset_types.h"
#include "asset_importer.h"
#include "gpu_mesh_buffer.h"
#include "texture_loader.h"

#include <wrl/client.h>

// Forward-declare D3D12MA types to avoid leaking the D3D12MemAlloc header.
namespace D3D12MA { class Allocator; class Allocation; }

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace mars
{

using Microsoft::WRL::ComPtr;

class DeviceContext;

// =============================================================================
// GpuModel — GPU-resident representation of a loaded model
// (one GpuMeshBuffer per MeshData primitive)
// =============================================================================
struct GpuModel
{
    std::string                    name;
    std::vector<GpuMeshBuffer>     mesh_buffers;  // parallel to ModelAsset::meshes
    std::vector<uint32_t>          texture_slots;  // SRV slot per material base-colour texture
    AABB                           bounds = {};
};

// =============================================================================
// ResourceManager
// =============================================================================
class MARS_ENGINE_API ResourceManager
{
public:
    ResourceManager()  = default;
    ~ResourceManager() { shutdown(); }

    ResourceManager(const ResourceManager&)            = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    // Initialise D3D12MA allocator.  Must be called after DeviceContext::init().
    void init(DeviceContext& ctx);
    void shutdown();

    // ---- Asset loading -------------------------------------------------------

    // Load a 3D model from disk and upload it to the GPU.
    // Returns the index of the GpuModel in the internal list, or UINT32_MAX on failure.
    uint32_t load_model(DeviceContext& ctx, const std::string& file_path);

    // Load a texture from disk and upload it to the GPU.
    // Returns the bindless SRV slot, or UINT32_MAX on failure.
    uint32_t load_texture(DeviceContext& ctx, const std::string& file_path, bool is_srgb = true);

    // ---- Accessors ----------------------------------------------------------
    GpuModel&        model(uint32_t index)   { return m_models[index]; }
    const GpuModel&  model(uint32_t index)   const { return m_models[index]; }
    uint32_t         model_count()           const { return static_cast<uint32_t>(m_models.size()); }

    GpuTexture&       texture(uint32_t index)  { return m_textures[index]; }
    const GpuTexture& texture(uint32_t index)  const { return m_textures[index]; }
    uint32_t          texture_count()          const { return static_cast<uint32_t>(m_textures.size()); }

    D3D12MA::Allocator* allocator() const { return m_allocator; }

private:
    D3D12MA::Allocator* m_allocator = nullptr;

    AssetImporter  m_importer;
    TextureLoader  m_tex_loader;

    std::vector<GpuModel>   m_models;
    std::vector<GpuTexture> m_textures;

    // Cache: file path → index in m_models / m_textures
    std::unordered_map<std::string, uint32_t> m_model_cache;
    std::unordered_map<std::string, uint32_t> m_texture_cache;

    bool m_initialised = false;
};

} // namespace mars
