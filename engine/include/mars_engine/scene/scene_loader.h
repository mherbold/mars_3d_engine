// =============================================================================
// scene_loader.h
// MARS 3D Engine — .marsscene JSON file parser
//
// SceneLoader parses a .marsscene JSON file and populates a Scene with all
// model instances, lights, and cameras described in it.
//
// Usage:
//   SceneLoader loader;
//   loader.load("path/to/scene.marsscene", ctx, resource_mgr, scene);
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "scene.h"

#include <string>

namespace mars
{

class DeviceContext;
class ResourceManager;

// =============================================================================
// SceneLoader
// =============================================================================
class MARS_ENGINE_API SceneLoader
{
public:
    SceneLoader()  = default;
    ~SceneLoader() = default;

    // Parse `file_path` (must have .marsscene extension) and populate `scene`.
    // Blocks until all models referenced in the file are loaded and uploaded.
    // Returns true on success; logs errors via std::println and returns false
    // if the file cannot be opened or parsed.
    bool load(const std::string& file_path,
              DeviceContext&     ctx,
              ResourceManager&   resource_mgr,
              Scene&             scene);
};

} // namespace mars
