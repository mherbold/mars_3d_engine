// =============================================================================
// asset_types.h
// MARS 3D Engine — Asset data structures
//
// Defines the CPU-side representations of meshes, materials, and models
// produced by the asset importer and consumed by the GPU upload pipeline.
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../math/math_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mars
{

// =============================================================================
// Vertex — interleaved layout matching the GPU structured buffer format
// position | normal | tangent | uv | bone_indices | bone_weights
// =============================================================================
struct Vertex
{
    Vec3     position    = {};
    Vec3     normal      = {};
    Vec3     tangent     = {};
    Vec2     uv          = {};
    uint32_t bone_indices[4] = {0, 0, 0, 0};
    float    bone_weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

// =============================================================================
// TextureRef — lightweight reference to a source texture on disk
// =============================================================================
struct TextureRef
{
    std::string path;        // Absolute or relative file path
    bool        is_srgb = true; // false for normal maps / roughness / metallic
};

// =============================================================================
// MaterialData — PBR material parameters
// =============================================================================
struct MaterialData
{
    std::string name;

    Vec4 base_color_factor   = {1.0f, 1.0f, 1.0f, 1.0f};
    float metallic_factor    = 0.0f;
    float roughness_factor   = 1.0f;
    Vec3  emissive_factor    = {};

    TextureRef base_color_texture;
    TextureRef normal_texture;
    TextureRef metallic_roughness_texture;
    TextureRef emissive_texture;
    TextureRef occlusion_texture;

    bool double_sided        = false;
    bool alpha_masked        = false;
    float alpha_cutoff       = 0.5f;
};

// =============================================================================
// MeshData — one primitive within a model (single material, single draw call)
// =============================================================================
struct MeshData
{
    std::string           name;
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    uint32_t              material_index = 0; // Index into ModelAsset::materials
    AABB                  bounds         = {};
};

// =============================================================================
// ModelAsset — the complete CPU-side representation of a loaded 3D model file
// =============================================================================
struct ModelAsset
{
    std::string                name;
    std::string                source_path;

    std::vector<MeshData>      meshes;
    std::vector<MaterialData>  materials;

    AABB                       bounds = {};
};

} // namespace mars
