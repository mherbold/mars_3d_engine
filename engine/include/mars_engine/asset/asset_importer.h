// =============================================================================
// asset_importer.h
// MARS 3D Engine — Assimp-based model importer
//
// Loads FBX, glTF 2.0, and OBJ files into CPU-side ModelAsset structures.
// Also imports skeletal animation data (Skeleton and AnimationClip).
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "asset_types.h"
#include "../animation/skeleton.h"
#include "../animation/animation_clip.h"

#include <string>
#include <optional>

namespace mars
{

// =============================================================================
// AssetImporter
// =============================================================================
class MARS_ENGINE_API AssetImporter
{
public:
    AssetImporter()  = default;
    ~AssetImporter() = default;

    // Non-copyable (stateless but keeps this consistent with other subsystems).
    AssetImporter(const AssetImporter&)            = delete;
    AssetImporter& operator=(const AssetImporter&) = delete;

    // Load a 3D model file (FBX / glTF / OBJ) and return a CPU-side ModelAsset.
    // Returns std::nullopt and logs an error on failure.
    //
    // `pre_transform_vertices`: when true, Assimp's aiProcess_PreTransformVertices
    // and aiProcess_GlobalScale are applied so all node transforms (including the
    // FBX UnitScaleFactor and per-submesh placement within the scene graph) are
    // baked into the vertex positions. This is required for assets that consist
    // of many independently-placed submeshes (e.g. SpeedTree vegetation), but it
    // strips bones, so it must NOT be used for skeletal-animation assets.
    std::optional<ModelAsset> import(const std::string& file_path,
                                     bool pre_transform_vertices = false) const;

    // Import skeleton data from a model file (if present).
    // Returns std::nullopt if the file has no skeletal data.
    std::optional<Skeleton> import_skeleton(const std::string& file_path) const;

    // Import all animation clips from a model file.
    // Returns an empty vector if the file has no animations.
    std::vector<AnimationClip> import_animations(const std::string& file_path) const;

    // Import a SpeedTree ORCA v2 vegetation asset from a species base directory.
    // Expects the following directory structure:
    //   species_path/HighPoly/<species>.fbx     — LOD 0 (Near)
    //   species_path/LowPoly/<species>.fbx      — LOD 1 (Mid)
    //   species_path/Textures/BaseColor.dds     — RGBA (RGB=color, A=opacity)
    //   species_path/Textures/Specular.dds      — RGB (R=occlusion, G=roughness, B=metalness)
    //   species_path/Textures/Normal.dds        — RGB (DirectX Y-up convention)
    //
    // Returns three ModelAsset instances: [0]=HighPoly (LOD 0), [1]=LowPoly (LOD 1), [2]=FarCluster (LOD 2, same as LowPoly for now)
    // Impostor atlas (LOD 3) is handled separately via an offline bake tool.
    std::vector<ModelAsset> import_vegetation_species(const std::string& species_path) const;

private:
    // Convert an Assimp material to a MaterialData.
    // (Forward-declared here; defined in the .cpp to avoid Assimp headers leaking.)
    struct ImportContext;
    static void process_node(ImportContext& ctx, const void* ai_node);
    static MeshData  process_mesh(ImportContext& ctx, const void* ai_mesh);
    static MaterialData process_material(ImportContext& ctx, uint32_t mat_index);
};

} // namespace mars
