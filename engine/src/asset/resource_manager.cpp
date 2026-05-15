// =============================================================================
// resource_manager.cpp
// MARS 3D Engine — GPU resource manager implementation
// =============================================================================

#include "mars_engine/asset/resource_manager.h"
#include "mars_engine/renderer/device_context.h"

#pragma warning(push, 0)
#include <D3D12MemAlloc.h>
#pragma warning(pop)

#include "mars_engine/engine_api.h"
#include <stdexcept>
#include <format>

namespace mars
{

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
void ResourceManager::init(DeviceContext& ctx)
{
    if (m_initialised) return;

    D3D12MA::ALLOCATOR_DESC desc{};
    desc.pDevice  = ctx.device();
    desc.pAdapter = ctx.adapter();
    desc.Flags    = D3D12MA::ALLOCATOR_FLAG_NONE;

    HRESULT hr = D3D12MA::CreateAllocator(&desc, &m_allocator);
    if (FAILED(hr))
        throw std::runtime_error(
            std::format("D3D12MA::CreateAllocator failed (HRESULT 0x{:08X})", static_cast<unsigned>(hr)));

    m_initialised = true;
    MARS_LOG("[ResourceManager] Initialised (D3D12MA allocator ready).");
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------
void ResourceManager::shutdown()
{
    if (!m_initialised) return;

    for (auto& m : m_models)
        for (auto& buf : m.mesh_buffers)
            buf.destroy();
    m_models.clear();

    for (auto& t : m_textures)
        t.destroy();
    m_textures.clear();

    m_model_cache.clear();
    m_texture_cache.clear();

    if (m_allocator) { m_allocator->Release(); m_allocator = nullptr; }
    m_initialised = false;
}

// ---------------------------------------------------------------------------
// load_model
// ---------------------------------------------------------------------------
uint32_t ResourceManager::load_model(DeviceContext& ctx, const std::string& file_path,
                                     bool pre_transform_vertices)
{
    // Return cached index if already loaded.
    auto it = m_model_cache.find(file_path);
    if (it != m_model_cache.end())
        return it->second;

    auto cpu_asset = m_importer.import(file_path, pre_transform_vertices);
    if (!cpu_asset)
    {
        MARS_LOG("[ResourceManager] load_model failed for '{}'", file_path);
        return UINT32_MAX;
    }

    GpuModel gpu_model;
    gpu_model.name   = cpu_asset->name;
    gpu_model.bounds = cpu_asset->bounds;

    // Upload each mesh primitive.
    gpu_model.mesh_buffers.reserve(cpu_asset->meshes.size());
    gpu_model.mesh_material_indices.reserve(cpu_asset->meshes.size());
    for (const auto& mesh : cpu_asset->meshes)
    {
        GpuMeshBuffer buf;
        buf.upload(ctx, m_allocator, mesh);
        gpu_model.mesh_buffers.push_back(std::move(buf));
        gpu_model.mesh_material_indices.push_back(mesh.material_index);
    }

    // Preserve CPU-side materials so callers (renderer) can read factors/flags.
    gpu_model.materials = cpu_asset->materials;

    // Load material textures (base colour, normal, metallic-roughness).
    gpu_model.texture_slots.reserve(cpu_asset->materials.size());
    gpu_model.normal_slots.reserve(cpu_asset->materials.size());
    gpu_model.mr_slots.reserve(cpu_asset->materials.size());
    for (const auto& mat : cpu_asset->materials)
    {
        if (!mat.base_color_texture.path.empty())
        {
            uint32_t slot = load_texture(ctx, mat.base_color_texture.path, mat.base_color_texture.is_srgb);
            gpu_model.texture_slots.push_back(slot);
        }
        else
        {
            // White 1×1 fallback.
            GpuTexture fallback = m_tex_loader.create_solid_color(ctx, m_allocator, 255, 255, 255, 255);
            uint32_t   slot     = fallback.srv_slot;
            m_textures.push_back(std::move(fallback));
            gpu_model.texture_slots.push_back(slot);
        }

        gpu_model.normal_slots.push_back(
            !mat.normal_texture.path.empty()
                ? load_texture(ctx, mat.normal_texture.path, mat.normal_texture.is_srgb)
                : UINT32_MAX);

        gpu_model.mr_slots.push_back(
            !mat.metallic_roughness_texture.path.empty()
                ? load_texture(ctx, mat.metallic_roughness_texture.path,
                               mat.metallic_roughness_texture.is_srgb)
                : UINT32_MAX);
    }

    // Import skeleton and animation clips (no-op if the model has none).
    {
        auto skel = m_importer.import_skeleton(file_path);
        if (skel && !skel->bones.empty())
        {
            gpu_model.skeleton = std::move(*skel);
            gpu_model.animation_clips = m_importer.import_animations(file_path);

            MARS_LOG("[ResourceManager] Skeleton loaded: {} bones, {} clip(s).",
                         gpu_model.skeleton.bones.size(),
                         gpu_model.animation_clips.size());
        }
    }

    uint32_t index = static_cast<uint32_t>(m_models.size());
    m_models.push_back(std::move(gpu_model));
    m_model_cache.emplace(file_path, index);

    MARS_LOG("[ResourceManager] Model '{}' loaded as index {}.", file_path, index);
    return index;
}
// ---------------------------------------------------------------------------
// load_texture
// ---------------------------------------------------------------------------
uint32_t ResourceManager::load_texture(DeviceContext& ctx, const std::string& file_path, bool is_srgb)
{
    auto it = m_texture_cache.find(file_path);
    if (it != m_texture_cache.end())
        return m_textures[it->second].srv_slot;

    GpuTexture tex = m_tex_loader.load(ctx, m_allocator, file_path, is_srgb);
    if (!tex.is_valid())
    {
        MARS_LOG("[ResourceManager] load_texture failed for '{}'", file_path);
        return UINT32_MAX;
    }

    uint32_t index = static_cast<uint32_t>(m_textures.size());
    uint32_t slot  = tex.srv_slot;
    m_textures.push_back(std::move(tex));
    m_texture_cache.emplace(file_path, index);
    return slot;
}

} // namespace mars
