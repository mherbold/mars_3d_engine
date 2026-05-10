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

} // namespace mars
