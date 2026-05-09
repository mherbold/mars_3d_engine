// =============================================================================
// scene.h
// MARS 3D Engine — Scene management public API
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../math/math_types.h"
#include "../asset/asset_types.h"
#include "../asset/resource_manager.h"

#include <string>
#include <vector>
#include <cstdint>

namespace mars
{

class DeviceContext;

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

    // Load all models referenced in the scene and upload them to the GPU.
    // `resource_mgr` must already be initialised.
    void load(DeviceContext& ctx, ResourceManager& resource_mgr, const std::string& scene_name);

    // Add a model instance to the scene.  Returns instance index.
    uint32_t add_model(const std::string& file_path,
                       DeviceContext&     ctx,
                       ResourceManager&  resource_mgr,
                       const Transform&  transform = {},
                       const std::string& name = {});

    void unload();

    // ---- Accessors ----------------------------------------------------------
    const std::string&                    name()      const { return m_name; }
    const std::vector<SceneModelInstance>& instances() const { return m_instances; }
    uint32_t                              instance_count() const
        { return static_cast<uint32_t>(m_instances.size()); }

private:
    std::string                    m_name;
    std::vector<SceneModelInstance> m_instances;
    bool m_loaded = false;
};

} // namespace mars
