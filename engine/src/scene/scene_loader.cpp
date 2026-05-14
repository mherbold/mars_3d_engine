// =============================================================================
// scene_loader.cpp
// MARS 3D Engine — .marsscene JSON file parser implementation
// =============================================================================

#include "mars_engine/engine_api.h"
#include "mars_engine/scene/scene_loader.h"
#include "mars_engine/scene/scene_types.h"
#include "mars_engine/asset/resource_manager.h"
#include "mars_engine/renderer/device_context.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <filesystem>
#include <cmath>
#include <windows.h>
#include <format>

using json = nlohmann::json;

namespace mars
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static Vec3 json_vec3(const json& j, Vec3 fallback = {})
{
    if (j.is_array() && j.size() >= 3)
        return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
    return fallback;
}

static Vec4 json_vec4(const json& j, Vec4 fallback = { 1,1,1,1 })
{
    if (j.is_array() && j.size() >= 4)
        return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() };
    if (j.is_array() && j.size() >= 3)
        return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), 1.0f };
    return fallback;
}

// Resolve a path relative to the scene file directory.
static std::string resolve_path(const std::string& base_dir, const std::string& path)
{
    if (std::filesystem::path(path).is_absolute())
        return path;
    return (std::filesystem::path(base_dir) / path).string();
}

// ---------------------------------------------------------------------------
// SceneLoader::load
// ---------------------------------------------------------------------------
bool SceneLoader::load(const std::string& file_path,
                        DeviceContext&     ctx,
                        ResourceManager&   resource_mgr,
                        Scene&             scene)
{
    // ---- Open and parse JSON ------------------------------------------------
    std::ifstream f(file_path);
    if (!f)
    {
        MARS_LOG("[SceneLoader] ERROR: Cannot open '{}'", file_path);
        return false;
    }

    json root;
    try
    {
        root = json::parse(f);
    }
    catch (const json::exception& e)
    {
        MARS_LOG("[SceneLoader] ERROR: JSON parse error in '{}': {}", file_path, e.what());
        return false;
    }

    const std::string base_dir = std::filesystem::path(file_path).parent_path().string();

    // "scene" block carries metadata (name only); all content sections
    // (skybox, cameras, lights, models) live at the root level.
    const json& meta = root.contains("scene") ? root["scene"] : root;
    const json& s    = root;

    const std::string scene_name = meta.value("name", file_path);
    MARS_LOG("[SceneLoader] Loading scene '{}' from '{}'", scene_name, file_path);

    scene.m_name   = scene_name;
    scene.m_wind   = {};
    scene.m_loaded = false;

    // ---- Wind ---------------------------------------------------------------
    // Supports two schemas:
    //   Legacy:  "wind": [x, y, z]                 — static Vec3, no animation
    //   Current: "wind": { "baseDirection": [...],  — animated WindDesc
    //                      "baseSpeed": 8.0, ... }
    if (s.contains("wind"))
    {
        const json& jw = s["wind"];
        if (jw.is_array())
        {
            // Legacy static vector — convert to WindDesc with no animation layers.
            Vec3 v = json_vec3(jw);
            float speed = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
            scene.m_wind      = v;
            scene.m_wind_desc.base_speed = speed;
            if (speed > 0.0f)
                scene.m_wind_desc.base_direction = { v.x/speed, v.y/speed, v.z/speed };
        }
        else if (jw.is_object())
        {
            WindDesc& wd = scene.m_wind_desc;

            if (jw.contains("baseDirection"))
            {
                Vec3 d = json_vec3(jw["baseDirection"], { 1.0f, 0.0f, 0.0f });
                float len = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
                if (len > 1e-6f)
                    wd.base_direction = { d.x/len, d.y/len, d.z/len };
            }

            wd.base_speed                 = jw.value("baseSpeed",                  0.0f);
            wd.gust_strength              = jw.value("gustStrength",               0.0f);
            wd.gust_frequency             = jw.value("gustFrequency",              0.25f);
            wd.direction_wander_angle     = jw.value("directionWanderAngle",       0.0f);
            wd.direction_wander_frequency = jw.value("directionWanderFrequency",   0.12f);
            wd.micro_variation            = jw.value("microVariation",             0.0f);
            wd.micro_frequency            = jw.value("microFrequency",             1.5f);
            wd.response_smoothing         = jw.value("responseSmoothing",          0.0f);

            // Pre-compute the base static wind vector so legacy Vec3 consumers still work.
            scene.m_wind = {
                wd.base_direction.x * wd.base_speed,
                wd.base_direction.y * wd.base_speed,
                wd.base_direction.z * wd.base_speed
            };
        }
    }

    // ---- Skybox -------------------------------------------------------------
    if (s.contains("skybox"))
    {
        const json& sky = s["skybox"];
        std::string type_str = sky.value("type", "physical");
        if (type_str == "hdri")
        {
            scene.m_skybox.type      = SkyboxDesc::Type::HDRI;
            scene.m_skybox.hdri_path = resolve_path(base_dir, sky.value("hdri", ""));
        }
        else
        {
            scene.m_skybox.type = SkyboxDesc::Type::Physical;
        }
        if (sky.contains("sun_direction"))
            scene.m_skybox.sun_direction = json_vec3(sky["sun_direction"], { 0.3f, 0.8f, 0.1f });
        scene.m_skybox.sun_intensity = sky.value("sun_intensity", 10.0f);
    }

    // ---- Lights -------------------------------------------------------------
    if (s.contains("lights"))
    {
        for (const auto& jl : s["lights"])
        {
            LightDesc light;
            std::string type_str = jl.value("type", "directional");
            if (type_str == "point")       light.type = LightType::Point;
            else if (type_str == "spot")   light.type = LightType::Spot;
            else                           light.type = LightType::Directional;

            if (jl.contains("direction")) light.direction = json_vec3(jl["direction"]);
            if (jl.contains("position"))  light.position  = json_vec3(jl["position"]);
            if (jl.contains("color"))     light.color     = json_vec3(jl["color"], { 1,1,1 });
            light.intensity        = jl.value("intensity",         1.0f);
            light.range            = jl.value("range",             0.0f);
            light.inner_angle_deg  = jl.value("inner_angle_deg",   0.0f);
            light.outer_angle_deg  = jl.value("outer_angle_deg",  45.0f);
            light.name             = jl.value("name",              "");
            scene.m_lights.push_back(light);
        }
    }

    // Default directional sun if no lights specified
    if (scene.m_lights.empty())
    {
        LightDesc sun;
        sun.type      = LightType::Directional;
        sun.direction = scene.m_skybox.sun_direction; // NOTE: must point TOWARD the sky (+Y up)
        sun.intensity = scene.m_skybox.sun_intensity;
        sun.color     = { 1.0f, 0.98f, 0.95f };
        sun.name      = "Sun";
        scene.m_lights.push_back(sun);
    }

    // ---- Cameras ------------------------------------------------------------
    if (s.contains("cameras"))
    {
        for (const auto& jc : s["cameras"])
        {
            CameraDesc cam;
            cam.id       = jc.value("id",      "camera");
            cam.fov_deg  = jc.value("fov",      90.0f);
            cam.near_z   = jc.value("near",      0.1f);
            cam.far_z    = jc.value("far",    10000.0f);
            if (jc.contains("position")) cam.position = json_vec3(jc["position"]);
            if (jc.contains("target"))   cam.target   = json_vec3(jc["target"],
                                                                   { cam.position.x,
                                                                     cam.position.y,
                                                                     cam.position.z - 1.0f });
            scene.m_cameras.push_back(cam);
        }
    }

    // Default camera if none specified
    if (scene.m_cameras.empty())
    {
        CameraDesc cam;
        cam.id      = "player_cam";
        cam.fov_deg = 90.0f;
        cam.near_z  = 0.1f;
        cam.far_z   = 10000.0f;
        scene.m_cameras.push_back(cam);
    }

    // ---- Models -------------------------------------------------------------
    uint32_t models_loaded = 0;
    uint32_t models_failed = 0;

    if (s.contains("models"))
    {
        for (const auto& jm : s["models"])
        {
            if (jm.value("enabled", true) == false)
            {
                const std::string skipped_name = jm.value("id", jm.value("file", "<unknown>"));
                MARS_LOG("[SceneLoader] Model '{}': disabled — skipped", skipped_name);
                continue;
            }

            if (!jm.contains("file"))
            {
                MARS_LOG("[SceneLoader] WARNING: model entry missing 'file' field — skipped");
                ++models_failed;
                continue;
            }

            const std::string rel_path    = jm["file"].get<std::string>();
            const std::string model_path  = resolve_path(base_dir, rel_path);
            const std::string model_name  = jm.value("id", rel_path);

            MARS_LOG("[SceneLoader] Model '{}': resolved path = '{}', exists = {}",
                         model_name, model_path,
                         std::filesystem::exists(model_path) ? "YES" : "NO");

            // Parse transform
            Transform transform;
            if (jm.contains("transform"))
            {
                const json& jt = jm["transform"];
                if (jt.contains("position"))
                {
                    Vec3 p = json_vec3(jt["position"]);
                    transform.position = p;
                }
                if (jt.contains("scale"))
                {
                    // Accept scalar or vec3 scale; use x component if vec3
                    if (jt["scale"].is_number())
                        transform.scale = jt["scale"].get<float>();
                    else if (jt["scale"].is_array() && !jt["scale"].empty())
                        transform.scale = jt["scale"][0].get<float>();
                }
                if (jt.contains("rotation"))
                {
                    // Rotation as Euler angles (degrees) [x, y, z]
                    Vec3 euler_deg = json_vec3(jt["rotation"]);
                    constexpr float k_deg2rad = 3.14159265358979323846f / 180.0f;
                    float rx = euler_deg.x * k_deg2rad * 0.5f;
                    float ry = euler_deg.y * k_deg2rad * 0.5f;
                    float rz = euler_deg.z * k_deg2rad * 0.5f;
                    // ZYX Euler → quaternion
                    Quaternion qx{ std::sin(rx), 0, 0, std::cos(rx) };
                    Quaternion qy{ 0, std::sin(ry), 0, std::cos(ry) };
                    Quaternion qz{ 0, 0, std::sin(rz), std::cos(rz) };
                    transform.rotation = qz * qy * qx;
                }
            }

            uint32_t inst_idx = scene.add_model(model_path, ctx, resource_mgr, transform, model_name);
            if (inst_idx == UINT32_MAX)
            {
                MARS_LOG("[SceneLoader] WARNING: Failed to load model '{}' — skipped", model_path);
                ++models_failed;
            }
            else
            {
                // Parse optional material override block
                if (jm.contains("material"))
                {
                    const json& jmat = jm["material"];
                    MaterialOverride& mo = scene.m_instances[inst_idx].material_override;

                    auto read_tex = [&](const char* key) -> std::string
                    {
                        if (jmat.contains(key) && jmat[key].is_string())
                        {
                            std::string p = jmat[key].get<std::string>();
                            return p.empty() ? std::string{} : resolve_path(base_dir, p);
                        }
                        return {};
                    };

                    mo.base_color_texture          = read_tex("base_color_texture");
                    mo.normal_texture              = read_tex("normal_texture");
                    mo.metallic_roughness_texture  = read_tex("metallic_roughness_texture");
                    mo.roughness_texture           = read_tex("roughness_texture");
                    mo.occlusion_texture           = read_tex("occlusion_texture");
                    mo.emissive_texture            = read_tex("emissive_texture");

                    if (jmat.contains("base_color") && jmat["base_color"].is_array())
                    {
                        Vec4 c = json_vec4(jmat["base_color"]);
                        mo.base_color_r = c.x;
                        mo.base_color_g = c.y;
                        mo.base_color_b = c.z;
                        mo.base_color_a = c.w;
                    }
                    mo.metallic_factor  = jmat.value("metallic_factor",  -1.0f);
                    mo.roughness_factor = jmat.value("roughness_factor", -1.0f);
                    mo.emissive_scale   = jmat.value("emissive_scale",   -1.0f);

                    if (mo.has_any())
                        MARS_LOG("[SceneLoader]   Material override applied to '{}'", model_name);
                }

                // Parse optional animation block
                if (jm.contains("animation"))
                {
                    const json& janim = jm["animation"];
                    AnimationDesc& anim = scene.m_instances[inst_idx].animation;
                    anim.enabled    = true;
                    anim.clip_name  = janim.value("clip",  std::string{});
                    anim.loop       = janim.value("loop",  true);
                    anim.speed      = janim.value("speed", 1.0f);
                    MARS_LOG("[SceneLoader]   Animation desc: clip='{}' loop={} speed={}",
                                 anim.clip_name.empty() ? "<first>" : anim.clip_name,
                                 anim.loop, anim.speed);
                }

                ++models_loaded;
            }
        }
    }

    // ---- Rigid nodes --------------------------------------------------------
    uint32_t rigid_loaded = 0;
    if (s.contains("rigid_nodes"))
    {
        for (const auto& jr : s["rigid_nodes"])
        {
            if (jr.value("enabled", true) == false) continue;

            if (!jr.contains("file"))
            {
                MARS_LOG("[SceneLoader] WARNING: rigid_node entry missing 'file' — skipped");
                continue;
            }

            const std::string model_path = resolve_path(base_dir, jr["file"].get<std::string>());
            const std::string node_name  = jr.value("id", model_path);
            const uint32_t    mesh_index = jr.value("mesh_index", 0u);

            // Parse transform
            Transform transform;
            if (jr.contains("transform"))
            {
                const json& jt = jr["transform"];
                if (jt.contains("position")) transform.position = json_vec3(jt["position"]);
                if (jt.contains("scale"))
                {
                    if (jt["scale"].is_number())
                        transform.scale = jt["scale"].get<float>();
                    else if (jt["scale"].is_array() && !jt["scale"].empty())
                        transform.scale = jt["scale"][0].get<float>();
                }
                if (jt.contains("rotation"))
                {
                    Vec3 euler_deg = json_vec3(jt["rotation"]);
                    constexpr float k = 3.14159265358979323846f / 180.0f;
                    float rx = euler_deg.x * k * 0.5f;
                    float ry = euler_deg.y * k * 0.5f;
                    float rz = euler_deg.z * k * 0.5f;
                    Quaternion qx{ std::sin(rx), 0, 0, std::cos(rx) };
                    Quaternion qy{ 0, std::sin(ry), 0, std::cos(ry) };
                    Quaternion qz{ 0, 0, std::sin(rz), std::cos(rz) };
                    transform.rotation = qz * qy * qx;
                }
            }

            // Parse animation
            std::string clip_name;
            bool        loop  = true;
            float       speed = 1.0f;
            if (jr.contains("animation"))
            {
                const json& ja = jr["animation"];
                clip_name = ja.value("clip",  std::string{});
                loop      = ja.value("loop",  true);
                speed     = ja.value("speed", 1.0f);
            }

            uint32_t idx = scene.add_rigid_node(model_path, ctx, resource_mgr,
                                                 mesh_index, transform,
                                                 clip_name, loop, speed, node_name);
            if (idx != UINT32_MAX)
            {
                ++rigid_loaded;
                MARS_LOG("[SceneLoader] Rigid node '{}': mesh={} clip='{}'",
                             node_name, mesh_index,
                             clip_name.empty() ? "<first>" : clip_name);
            }
        }
    }

    // ---- Cloth simulation instances -----------------------------------------
    uint32_t cloth_loaded = 0;
    if (s.contains("cloth"))
    {
        for (const auto& jc : s["cloth"])
        {
            if (jc.value("enabled", true) == false) continue;

            const std::string cloth_name = jc.value("id", std::string("cloth"));

            // Transform
            Transform transform;
            if (jc.contains("transform"))
            {
                const json& jt = jc["transform"];
                if (jt.contains("position")) transform.position = json_vec3(jt["position"]);
                if (jt.contains("scale"))
                {
                    if (jt["scale"].is_number())
                        transform.scale = jt["scale"].get<float>();
                    else if (jt["scale"].is_array() && !jt["scale"].empty())
                        transform.scale = jt["scale"][0].get<float>();
                }
                if (jt.contains("rotation"))
                {
                    Vec3 euler_deg = json_vec3(jt["rotation"]);
                    constexpr float k = 3.14159265358979323846f / 180.0f;
                    float rx = euler_deg.x * k * 0.5f;
                    float ry = euler_deg.y * k * 0.5f;
                    float rz = euler_deg.z * k * 0.5f;
                    Quaternion qx{ std::sin(rx), 0, 0, std::cos(rx) };
                    Quaternion qy{ 0, std::sin(ry), 0, std::cos(ry) };
                    Quaternion qz{ 0, 0, std::sin(rz), std::cos(rz) };
                    transform.rotation = qz * qy * qx;
                }
            }

            // Cloth simulation parameters
            ClothDesc desc;
            desc.grid_w                  = jc.value("grid_w",                   16u);
            desc.grid_h                  = jc.value("grid_h",                   16u);
            desc.mass                    = jc.value("mass",                      0.1f);
            desc.structural_compliance   = jc.value("structural_compliance",   1e-7f);
            desc.shear_compliance        = jc.value("shear_compliance",        1e-6f);
            desc.bend_compliance         = jc.value("bend_compliance",         1e-4f);
            desc.damping                 = jc.value("damping",                 0.99f);
            desc.xpbd_iterations         = jc.value("xpbd_iterations",         10u);
            desc.pin_corners             = jc.value("pin_corners",               0b0011u);

            // Optional material override
            if (jc.contains("material"))
            {
                const json& jmat = jc["material"];
                MaterialOverride& mo = desc.material;

                auto read_tex = [&](const char* key) -> std::string
                {
                    if (jmat.contains(key) && jmat[key].is_string())
                    {
                        std::string p = jmat[key].get<std::string>();
                        return p.empty() ? std::string{} : resolve_path(base_dir, p);
                    }
                    return {};
                };

                mo.base_color_texture         = read_tex("base_color_texture");
                mo.normal_texture             = read_tex("normal_texture");
                mo.metallic_roughness_texture = read_tex("metallic_roughness_texture");
                mo.roughness_texture          = read_tex("roughness_texture");
                mo.occlusion_texture          = read_tex("occlusion_texture");
                mo.emissive_texture           = read_tex("emissive_texture");

                if (jmat.contains("base_color") && jmat["base_color"].is_array())
                {
                    Vec4 c = json_vec4(jmat["base_color"]);
                    mo.base_color_r = c.x;
                    mo.base_color_g = c.y;
                    mo.base_color_b = c.z;
                    mo.base_color_a = c.w;
                }
                mo.metallic_factor  = jmat.value("metallic_factor",  -1.0f);
                mo.roughness_factor = jmat.value("roughness_factor", -1.0f);
                mo.emissive_scale   = jmat.value("emissive_scale",   -1.0f);
            }

            uint32_t idx = scene.add_cloth(transform, desc, cloth_name);
            if (idx != UINT32_MAX)
            {
                ++cloth_loaded;
                MARS_LOG("[SceneLoader] Cloth '{}': {}x{} grid", cloth_name, desc.grid_w, desc.grid_h);
            }
        }
    }

    MARS_LOG("[SceneLoader] Scene '{}' loaded: {} model(s), {} rigid node(s), {} cloth(s), {} light(s), {} camera(s){} ",
                 scene_name, models_loaded, rigid_loaded, cloth_loaded, scene.m_lights.size(), scene.m_cameras.size(),
                 models_failed > 0
                     ? std::format(" ({} model(s) failed)", models_failed)
                     : std::string{});

    scene.m_loaded = true;
    return true;
}

} // namespace mars
