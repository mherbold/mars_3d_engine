// =============================================================================
// scene_types.h
// MARS 3D Engine — Scene data types (lights, cameras) parsed from .marsscene
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../math/math_types.h"
#include "../asset/gpu_mesh_buffer.h"

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

// =============================================================================
// WindDesc — animated wind parameters loaded from the scene file.
//
// The wind model layers three oscillators on top of a base direction + speed:
//
//   1. Gust      — slow amplitude modulation of speed
//                  (gustStrength × baseSpeed, at gustFrequency Hz)
//   2. Direction wander — slow angular drift of the wind direction in the
//                  horizontal plane (±directionWanderAngle degrees,
//                  at directionWanderFrequency Hz)
//   3. Micro     — high-frequency turbulence added to the final vector
//                  (microVariation as a fraction of current speed,
//                  at microFrequency Hz)
//
// All three contributions are summed then passed through a one-pole IIR
// low-pass smoother (responseSmoothing in [0,1); 0 = no smoothing,
// values close to 1 = very sluggish response) before being handed to
// the cloth solver each substep.
//
// All oscillator fields default to zero, so an old scene that only
// specifies a static wind vector continues to work unchanged.
// =============================================================================
struct WindDesc
{
    // Base wind — direction is normalised at load time; speed in m/s.
    Vec3  base_direction            = { 1.0f, 0.0f, 0.0f };
    float base_speed                = 0.0f;

    // Gust layer (amplitude oscillator on wind speed).
    float gust_strength             = 0.0f;  // fraction of base_speed (e.g. 0.45 = ±45 %)
    float gust_frequency            = 0.25f; // Hz

    // Direction-wander layer (slow horizontal angular drift).
    float direction_wander_angle    = 0.0f;  // degrees (half-range of the oscillation)
    float direction_wander_frequency= 0.12f; // Hz

    // Micro-turbulence layer (per-axis high-frequency noise on the final vector).
    float micro_variation           = 0.0f;  // fraction of current speed
    float micro_frequency           = 1.5f;  // Hz

    // One-pole IIR low-pass smoother coefficient in [0, 1).
    // output = mix(new_value, prev_output, responseSmoothing)
    // 0 = instant response; 0.85 = heavy smoothing.
    float response_smoothing        = 0.0f;
};

// =============================================================================
// AnimationDesc — optional animation settings for a SceneModelInstance
// Parsed from the "animation" block in a .marsscene instance entry.
// =============================================================================
struct AnimationDesc
{
    std::string clip_name;          // Animation clip to play (empty = first clip)
    bool        loop        = true;
    float       speed       = 1.0f;
    bool        enabled     = false; // Set to true when this block is present in the scene file

    bool has_animation() const { return enabled; }
};

// =============================================================================
// RigidNodeInstance — a scene object driven by a transform-only animation clip.
// Used for wheels, doors, rotating flags, etc. — no GPU skinning required.
// The clip provides a time-varying local transform (translation + rotation + scale)
// that is applied on top of the instance's base transform each frame.
// =============================================================================
struct RigidNodeInstance
{
    uint32_t    model_index     = UINT32_MAX;  // index into ResourceManager::m_models
    uint32_t    mesh_index      = 0;           // which mesh primitive within the model
    uint32_t    tlas_instance   = UINT32_MAX;  // PathTracer TLAS slot
    uint32_t    blas_index      = UINT32_MAX;  // PathTracer BLAS index (static, never refitted)
    Transform   base_transform  = {};          // world-space base pose
    std::string name;

    // Animation (CPU-evaluated each frame — no GPU skinning).
    uint32_t    anim_state_id   = UINT32_MAX;  // AnimationSystem state ID (UINT32_MAX = none)
    std::string clip_name;                     // Clip to play (empty = first)
    bool        loop            = true;
    float       speed           = 1.0f;

    // Current world transform (updated every frame by Renderer::update()).
    Mat4x4      current_world   = Mat4x4::identity();
};

// =============================================================================
// ClothDesc — parameters for a cloth simulation instance loaded from .marsscene
// =============================================================================
struct ClothDesc
{
    uint32_t grid_w = 16;            // cloth grid columns
    uint32_t grid_h = 16;            // cloth grid rows
    float    mass                  = 0.1f;   // kg per particle
    float    structural_compliance = 1e-7f;  // m/N — lower = stiffer
    float    shear_compliance      = 1e-6f;
    float    bend_compliance       = 1e-4f;
    float    damping               = 0.99f;
    uint32_t xpbd_iterations       = 10;

    // Bitmask controlling which particles are pinned (fixed, zero inverse-mass).
    //
    // Bits 0-3  — individual corners:
    //   Bit 0 = top-left,    Bit 1 = top-right
    //   Bit 2 = bottom-left, Bit 3 = bottom-right
    //
    // Bits 4-7  — full edges (entire row or column):
    //   Bit 4 = top edge    (all particles in row 0)
    //   Bit 5 = bottom edge (all particles in last row)
    //   Bit 6 = left edge   (all particles in column 0)
    //   Bit 7 = right edge  (all particles in last column)
    //
    // Bits may be combined freely.  Default (0b0011 = 3) pins TL + TR corners,
    // which is suitable for a curtain or banner hanging from two top points.
    uint32_t pin_corners = 0b0011u;

    // Optional material override (same semantics as SceneModelInstance::material_override)
    MaterialOverride material;
};

// =============================================================================
// ClothInstance — a cloth simulation object placed in the scene.
// The cloth mesh is a dynamically simulated regular grid; the top row of
// particles is pinned (fixed) so the cloth hangs and waves like a flag.
// =============================================================================
struct ClothInstance
{
    Transform   base_transform  = {};           // world-space anchor pose
    ClothDesc   cloth_desc      = {};           // simulation parameters
    std::string name;

    // GPU resources (set by Renderer::load_scene — not parsed from file)
    uint32_t    cloth_blas_index  = UINT32_MAX; // refittable BLAS index
    uint32_t    tlas_instance     = UINT32_MAX; // PathTracer TLAS slot
    uint32_t    mat_index         = 0;          // material slot for this cloth

    // Cloth GPU buffers (ping-pong positions/velocities + output vertex buffer)
    GpuMeshBuffer      mesh_buffer;   // static rest-pose grid mesh (used for BLAS geometry and DXR)
    ClothGpuResources  gpu;           // dynamic compute buffers

    // Rest lengths (computed at enable_cloth time from grid dimensions and scale)
    float       rest_len_struct = 0.1f;
    float       rest_len_shear  = 0.141f;
    float       rest_len_bend   = 0.2f;

    // Index buffer stored alongside the BLAS
    uint32_t    vertex_count    = 0;
    uint32_t    index_count     = 0;
};

} // namespace mars
