// =============================================================================
// scene.cpp
// MARS 3D Engine — Scene management implementation
// =============================================================================

#include "mars_engine/scene/scene.h"
#include "mars_engine/scene/scene_loader.h"
#include "mars_engine/renderer/device_context.h"

#include <print>

namespace mars
{

bool Scene::load_from_file(const std::string& file_path,
                            DeviceContext&     ctx,
                            ResourceManager&   resource_mgr)
{
    unload();
    SceneLoader loader;
    return loader.load(file_path, ctx, resource_mgr, *this);
}

void Scene::load(DeviceContext& /*ctx*/, ResourceManager& /*resource_mgr*/, const std::string& scene_name)
{
    m_name   = scene_name;
    m_loaded = true;
    std::println("[Scene] '{}' initialised.", m_name);
}

uint32_t Scene::add_model(
    const std::string& file_path,
    DeviceContext&     ctx,
    ResourceManager&   resource_mgr,
    const Transform&   transform,
    const std::string& name)
{
    uint32_t model_index = resource_mgr.load_model(ctx, file_path);
    if (model_index == UINT32_MAX)
        return UINT32_MAX;

    SceneModelInstance inst;
    inst.model_index = model_index;
    inst.transform   = transform;
    inst.name        = name.empty() ? file_path : name;

    uint32_t inst_index = static_cast<uint32_t>(m_instances.size());
    m_instances.push_back(inst);
    return inst_index;
}

void Scene::unload()
{
    m_instances.clear();
    m_lights.clear();
    m_cameras.clear();
    m_skybox  = {};
    m_loaded  = false;
}

} // namespace mars
