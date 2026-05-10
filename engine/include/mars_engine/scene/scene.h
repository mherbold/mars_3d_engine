// =============================================================================
// scene.h
// MARS 3D Engine — Scene management public API
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../math/math_types.h"
#include "../asset/asset_types.h"
#include "../asset/resource_manager.h"
#include "scene_types.h"

#include <string>
#include <vector>
#include <cstdint>

namespace mars
{

class DeviceContext;
class SceneLoader;

// =============================================================================
// SceneModelInstance — a placed instance of a loaded GPU model
// =============================================================================
struct SceneModelInstance
{
    uint32_t  model_index = UINT32_MAX; // index into ResourceManager::m_models
    Transform transform   = {};
    std::string name;
};

// =============================================================================
// Scene
// =============================================================================
class MARS_ENGINE_API Scene
{
public:
    Scene()  = default;
    ~Scene() { unload(); }

    Scene(const Scene&)            = delete;
    Scene& operator=(const Scene&) = delete;

    // Parse a .marsscene file and load all referenced assets onto the GPU.
    // This is the primary entry point for M5+.
    bool load_from_file(const std::string& file_path,
                        DeviceContext&     ctx,
                        ResourceManager&   resource_mgr);

    // Initialise an empty scene with a given name (used in tests / procedural scenes).
    void load(DeviceContext& ctx, ResourceManager& resource_mgr, const std::string& scene_name);

    // Add a model instance to the scene.  Returns instance index.
    uint32_t add_model(const std::string& file_path,
                       DeviceContext&     ctx,
                       ResourceManager&  resource_mgr,
                       const Transform&  transform = {},
                       const std::string& name = {});

    void unload();

    // ---- Accessors ----------------------------------------------------------
    const std::string&                     name()           const { return m_name; }
    const std::vector<SceneModelInstance>& instances()      const { return m_instances; }
    uint32_t                               instance_count() const
        { return static_cast<uint32_t>(m_instances.size()); }

    const std::vector<LightDesc>&          lights()         const { return m_lights; }
    const std::vector<CameraDesc>&         cameras()        const { return m_cameras; }
    const SkyboxDesc&                      skybox()         const { return m_skybox; }

    bool is_loaded() const { return m_loaded; }

private:
    friend class SceneLoader;

    std::string                     m_name;
    std::vector<SceneModelInstance> m_instances;
    std::vector<LightDesc>          m_lights;
    std::vector<CameraDesc>         m_cameras;
    SkyboxDesc                      m_skybox;
    bool                            m_loaded = false;
};

} // namespace mars
