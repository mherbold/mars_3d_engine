// =============================================================================
// scene_loader.cpp
// MARS 3D Engine — .marsscene JSON file parser implementation
// =============================================================================

#include "mars_engine/scene/scene_loader.h"
#include "mars_engine/scene/scene_types.h"
#include "mars_engine/asset/resource_manager.h"
#include "mars_engine/renderer/device_context.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <print>
#include <filesystem>

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
        std::println("[SceneLoader] ERROR: Cannot open '{}'", file_path);
        return false;
    }

    json root;
    try
    {
        root = json::parse(f);
    }
    catch (const json::exception& e)
    {
        std::println("[SceneLoader] ERROR: JSON parse error in '{}': {}", file_path, e.what());
        return false;
    }

    const std::string base_dir = std::filesystem::path(file_path).parent_path().string();

    // "scene" block carries metadata (name only); all content sections
    // (skybox, cameras, lights, models) live at the root level.
    const json& meta = root.contains("scene") ? root["scene"] : root;
    const json& s    = root;

    const std::string scene_name = meta.value("name", file_path);
    std::println("[SceneLoader] Loading scene '{}' from '{}'", scene_name, file_path);

    scene.m_name   = scene_name;
    scene.m_loaded = false;

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
            if (!jm.contains("file"))
            {
                std::println("[SceneLoader] WARNING: model entry missing 'file' field — skipped");
                ++models_failed;
                continue;
            }

            const std::string rel_path    = jm["file"].get<std::string>();
            const std::string model_path  = resolve_path(base_dir, rel_path);
            const std::string model_name  = jm.value("id", rel_path);

            std::println("[SceneLoader] Model '{}': resolved path = '{}', exists = {}",
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
                std::println("[SceneLoader] WARNING: Failed to load model '{}' — skipped", model_path);
                ++models_failed;
            }
            else
            {
                ++models_loaded;
            }
        }
    }

    std::println("[SceneLoader] Scene '{}' loaded: {} model(s), {} light(s), {} camera(s){} ",
                 scene_name, models_loaded, scene.m_lights.size(), scene.m_cameras.size(),
                 models_failed > 0
                     ? std::format(" ({} model(s) failed)", models_failed)
                     : std::string{});

    scene.m_loaded = true;
    return true;
}

} // namespace mars
