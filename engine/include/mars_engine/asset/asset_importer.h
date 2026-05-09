// =============================================================================
// asset_importer.h
// MARS 3D Engine — Assimp-based model importer
//
// Loads FBX, glTF 2.0, and OBJ files into CPU-side ModelAsset structures.
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "asset_types.h"

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
    std::optional<ModelAsset> import(const std::string& file_path) const;

private:
    // Convert an Assimp material to a MaterialData.
    // (Forward-declared here; defined in the .cpp to avoid Assimp headers leaking.)
    struct ImportContext;
    static void process_node(ImportContext& ctx, const void* ai_node);
    static MeshData  process_mesh(ImportContext& ctx, const void* ai_mesh);
    static MaterialData process_material(ImportContext& ctx, uint32_t mat_index);
};

} // namespace mars
