// =============================================================================
// camera.h
// MARS 3D Engine — Camera public API
//
// Camera computes view and projection matrices from position / look-at / FOV.
// It also maintains previous-frame matrices for motion-vector generation and
// a Halton jitter sequence for DLSS temporal accumulation.
//
// Coordinate system: right-handed, Y-up (consistent with D3D12 / HLSL NDC).
// Matrices are row-major to match HLSL row_major float4x4.
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../math/math_types.h"

namespace mars
{

// =============================================================================
// Camera
// =============================================================================
class MARS_ENGINE_API Camera
{
public:
    Camera() = default;

    // ---- Setup --------------------------------------------------------------

    void set_position(const Vec3& pos);
    void set_look_at(const Vec3& target, const Vec3& up = { 0.0f, 1.0f, 0.0f });
    void set_projection(float fov_horizontal_deg, float aspect_ratio,
                        float near_z, float far_z);

    // Apply a yaw/pitch offset on top of the current look direction.
    // Used by DisplayManager for surround / overhead multi-monitor setups.
    void set_display_offset(float yaw_deg, float pitch_deg, float roll_deg);

    // ---- Per-frame update ---------------------------------------------------

    // Call once per frame before handing matrices to the PathTracer.
    // Saves current matrices as previous-frame and applies jitter.
    void advance_frame(uint32_t frame_index);

    // ---- Accessors ----------------------------------------------------------

    const Vec3&   position()         const { return m_position; }
    const Mat4x4& view()             const { return m_view; }
    const Mat4x4& view_inv()         const { return m_view_inv; }
    const Mat4x4& proj()             const { return m_proj; }
    const Mat4x4& proj_inv()         const { return m_proj_inv; }
    const Mat4x4& prev_view()        const { return m_prev_view; }
    const Mat4x4& prev_proj()        const { return m_prev_proj; }

    float near_z()  const { return m_near_z; }
    float far_z()   const { return m_far_z; }

private:
    void rebuild_view();
    void rebuild_proj();

    Vec3  m_position    = {};
    Vec3  m_forward     = { 0.0f, 0.0f, -1.0f };
    Vec3  m_right       = { 1.0f, 0.0f,  0.0f };
    Vec3  m_up          = { 0.0f, 1.0f,  0.0f };

    float m_fov_h_deg   = 90.0f;
    float m_aspect      = 16.0f / 9.0f;
    float m_near_z      = 0.1f;
    float m_far_z       = 10000.0f;

    float m_yaw_offset_deg   = 0.0f;
    float m_pitch_offset_deg = 0.0f;
    float m_roll_offset_deg  = 0.0f;

    Mat4x4 m_view      = Mat4x4::identity();
    Mat4x4 m_view_inv  = Mat4x4::identity();
    Mat4x4 m_proj      = Mat4x4::identity();
    Mat4x4 m_proj_inv  = Mat4x4::identity();

    Mat4x4 m_prev_view = Mat4x4::identity();
    Mat4x4 m_prev_proj = Mat4x4::identity();
};

// =============================================================================
// FlyCamera — WASD + mouse-look free camera for the test application
// =============================================================================
class MARS_ENGINE_API FlyCamera
{
public:
    FlyCamera() = default;

    void init(const Vec3& position, float yaw_deg, float pitch_deg,
              float fov_h_deg, float aspect, float near_z, float far_z);

    // Call once per frame with raw input deltas.
    // `dt`       — frame delta-time in seconds
    // `move`     — WASD movement intent: x = right, y = up, z = forward
    // `mouse_dx` — horizontal mouse delta in pixels
    // `mouse_dy` — vertical mouse delta in pixels
    void update(float dt, Vec3 move, float mouse_dx, float mouse_dy);

    void set_aspect(float aspect);

    Camera&       camera()       { return m_camera; }
    const Camera& camera() const { return m_camera; }

    float move_speed   = 5.0f;   // metres per second
    float mouse_sens   = 0.15f;  // degrees per pixel

private:
    Camera m_camera;
    float  m_yaw_deg   = -90.0f;
    float  m_pitch_deg =   0.0f;
    float  m_fov_h_deg =  90.0f;
    float  m_near_z    =   0.1f;
    float  m_far_z     = 10000.0f;
};

} // namespace mars
