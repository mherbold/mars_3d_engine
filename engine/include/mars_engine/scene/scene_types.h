// =============================================================================
// scene_types.h
// MARS 3D Engine — Scene data types (lights, cameras) parsed from .marsscene
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../math/math_types.h"

#include <string>

namespace mars
{

// =============================================================================
// LightType
// =============================================================================
enum class LightType : uint8_t
{
    Directional = 0,
    Point       = 1,
    Spot        = 2,
};

// =============================================================================
// LightDesc — a light as loaded from the scene file
// =============================================================================
struct LightDesc
{
    LightType type      = LightType::Directional;
    Vec3      direction = { 0.3f, 0.8f, 0.1f };   // used for Directional / Spot
    Vec3      position  = {};                       // used for Point / Spot
    Vec3      color     = { 1.0f, 0.98f, 0.95f };  // linear RGB
    float     intensity = 1.0f;                     // multiplier (physical units: lux / candela)
    float     range     = 0.0f;                     // Point / Spot attenuation range (0 = infinite)
    float     inner_angle_deg = 0.0f;               // Spot inner cone half-angle
    float     outer_angle_deg = 45.0f;              // Spot outer cone half-angle
    std::string name;
};

// =============================================================================
// CameraDesc — a camera as loaded from the scene file
// =============================================================================
struct CameraDesc
{
    std::string id;
    Vec3        position  = {};
    Vec3        target    = { 0.0f, 0.0f, -1.0f };  // look-at target
    float       fov_deg   = 90.0f;                   // horizontal FOV
    float       near_z    = 0.1f;
    float       far_z     = 10000.0f;
};

// =============================================================================
// SkyboxDesc — sky configuration from the scene file
// =============================================================================
struct SkyboxDesc
{
    enum class Type : uint8_t { Physical = 0, HDRI = 1 };

    Type        type           = Type::Physical;
    Vec3        sun_direction  = { 0.3f, 0.8f, 0.1f };
    float       sun_intensity  = 10.0f;
    std::string hdri_path;                           // used when type == HDRI
};

// =============================================================================
// MaterialOverride â€" per-instance material override specified in .marsscene
//
// Any field left at its default (empty string / negative factor) means "use
// whatever the model's own material provides".  Set a path to replace that
// texture slot; set a factor >= 0 to replace the scalar factor.
// =============================================================================
struct MaterialOverride
{
    // Texture path overrides (empty = no override)
    std::string base_color_texture;
    std::string normal_texture;
    std::string metallic_roughness_texture;
    std::string roughness_texture;       // single-channel roughness (no metallic)
    std::string occlusion_texture;
    std::string emissive_texture;

    // Scalar factor overrides (negative = no override, use model default)
    float base_color_r    = -1.0f;
    float base_color_g    = -1.0f;
    float base_color_b    = -1.0f;
    float base_color_a    = -1.0f;
    float metallic_factor = -1.0f;
    float roughness_factor= -1.0f;
    float emissive_scale  = -1.0f;

    bool has_any() const
    {
        return !base_color_texture.empty()
            || !normal_texture.empty()
            || !metallic_roughness_texture.empty()
            || !roughness_texture.empty()
            || !occlusion_texture.empty()
            || !emissive_texture.empty()
            || base_color_r    >= 0.0f
            || metallic_factor >= 0.0f
            || roughness_factor>= 0.0f
            || emissive_scale  >= 0.0f;
    }
};

} // namespace mars
