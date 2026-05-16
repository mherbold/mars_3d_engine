// =============================================================================
// scene.cpp
// MARS 3D Engine — Scene management implementation
// =============================================================================

#include "mars_engine/engine_api.h"
#include "mars_engine/scene/scene.h"
#include "mars_engine/scene/scene_loader.h"
#include "mars_engine/renderer/device_context.h"


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
    MARS_LOG("[Scene] '{}' initialised.", m_name);
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
    m_rigid_nodes.clear();
    m_cloth_instances.clear();
    m_lights.clear();
    m_cameras.clear();
    m_skybox    = {};
    m_loaded    = false;
}

uint32_t Scene::add_rigid_node(
    const std::string& model_path,
    DeviceContext&     ctx,
    ResourceManager&   resource_mgr,
    uint32_t           mesh_index,
    const Transform&   base_transform,
    const std::string& clip_name,
    bool               loop,
    float              speed,
    const std::string& name)
{
    uint32_t model_index = resource_mgr.load_model(ctx, model_path);
    if (model_index == UINT32_MAX)
    {
        MARS_LOG("[Scene] add_rigid_node: failed to load model '{}'", model_path);
        return UINT32_MAX;
    }

    RigidNodeInstance rn;
    rn.model_index     = model_index;
    rn.base_transform  = base_transform;
    rn.current_world   = base_transform.to_matrix();
    rn.mesh_index      = mesh_index;
    rn.clip_name       = clip_name;
    rn.loop            = loop;
    rn.speed           = speed;
    rn.name            = name.empty() ? model_path : name;

    uint32_t idx = static_cast<uint32_t>(m_rigid_nodes.size());
    m_rigid_nodes.push_back(rn);
    return idx;
}

uint32_t Scene::add_cloth(
    const Transform&   base_transform,
    const ClothDesc&   desc,
    const std::string& name)
{
    ClothInstance ci;
    ci.base_transform = base_transform;
    ci.cloth_desc     = desc;
    ci.name           = name.empty() ? "cloth" : name;
    ci.vertex_count   = desc.grid_w * desc.grid_h;

    // Index count: (grid_h-1) * (grid_w-1) * 2 triangles * 3 indices each
    const uint32_t quad_h = (desc.grid_h > 0) ? desc.grid_h - 1 : 0;
    const uint32_t quad_w = (desc.grid_w > 0) ? desc.grid_w - 1 : 0;
    ci.index_count = quad_h * quad_w * 6u;

    uint32_t idx = static_cast<uint32_t>(m_cloth_instances.size());
    MARS_LOG("[Scene] Cloth '{}': {}x{} grid ({} verts, {} indices)",
                 ci.name, desc.grid_w, desc.grid_h, ci.vertex_count, ci.index_count);
    m_cloth_instances.push_back(std::move(ci));
    return idx;
}

} // namespace mars
