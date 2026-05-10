// =============================================================================
// camera.cpp
// MARS 3D Engine — Camera implementation
// =============================================================================

#include "mars_engine/camera/camera.h"

#include <cmath>
#include <algorithm>

namespace mars
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static constexpr float k_pi        = 3.14159265358979323846f;
static constexpr float k_deg2rad   = k_pi / 180.0f;
static constexpr float k_epsilon   = 1e-6f;

// Invert a rigid-body (rotation + translation) 4x4 matrix.
// Avoids a full Gauss-Jordan inverse by exploiting the structure:
//   M = [R | t]    M^-1 = [R^T | -R^T * t]
//       [0 | 1]            [0   |  1      ]
static Mat4x4 invert_rigid(const Mat4x4& m)
{
    Mat4x4 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = m.m[j][i];
    r.m[0][3] = -(r.m[0][0]*m.m[0][3] + r.m[0][1]*m.m[1][3] + r.m[0][2]*m.m[2][3]);
    r.m[1][3] = -(r.m[1][0]*m.m[0][3] + r.m[1][1]*m.m[1][3] + r.m[1][2]*m.m[2][3]);
    r.m[2][3] = -(r.m[2][0]*m.m[0][3] + r.m[2][1]*m.m[1][3] + r.m[2][2]*m.m[2][3]);
    r.m[3][3] = 1.0f;
    return r;
}

// Invert a D3D12 row-major perspective projection matrix analytically.
static Mat4x4 invert_proj(const Mat4x4& p)
{
    // Standard D3D12 perspective (depth [0,1], row-major):
    //  [a  0  0  0]   row 0
    //  [0  b  0  0]   row 1
    //  [0  0  c  d]   row 2   (d = -near*far/(far-near))
    //  [0  0  1  0]   row 3
    //
    // Inverse:
    //  [1/a  0    0    0   ]
    //  [0    1/b  0    0   ]
    //  [0    0    0    1   ]
    //  [0    0    1/d  -c/d]
    Mat4x4 r;
    float a = p.m[0][0];
    float b = p.m[1][1];
    float c = p.m[2][2];
    float d = p.m[2][3];

    if (std::abs(a) > k_epsilon) r.m[0][0] = 1.0f / a;
    if (std::abs(b) > k_epsilon) r.m[1][1] = 1.0f / b;
    r.m[2][3] = 1.0f;
    if (std::abs(d) > k_epsilon)
    {
        r.m[3][2] = 1.0f / d;
        r.m[3][3] = -c / d;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Camera::set_position
// ---------------------------------------------------------------------------
void Camera::set_position(const Vec3& pos)
{
    m_position = pos;
    rebuild_view();
}

// ---------------------------------------------------------------------------
// Camera::set_look_at
// ---------------------------------------------------------------------------
void Camera::set_look_at(const Vec3& target, const Vec3& up)
{
    Vec3 fwd = (target - m_position).normalized();
    if (fwd.length() < k_epsilon) return;

    m_forward = fwd;
    m_right   = m_forward.cross(up).normalized();
    if (m_right.length() < k_epsilon)
    {
        Vec3 alt_up = { 0.0f, 0.0f, 1.0f };
        m_right = m_forward.cross(alt_up).normalized();
    }
    m_up = m_right.cross(m_forward).normalized();

    rebuild_view();
}

// ---------------------------------------------------------------------------
// Camera::set_projection
// ---------------------------------------------------------------------------
void Camera::set_projection(float fov_horizontal_deg, float aspect_ratio,
                             float near_z, float far_z)
{
    m_fov_h_deg = fov_horizontal_deg;
    m_aspect    = aspect_ratio;
    m_near_z    = near_z;
    m_far_z     = far_z;
    rebuild_proj();
}

// ---------------------------------------------------------------------------
// Camera::set_display_offset
// ---------------------------------------------------------------------------
void Camera::set_display_offset(float yaw_deg, float pitch_deg, float roll_deg)
{
    m_yaw_offset_deg   = yaw_deg;
    m_pitch_offset_deg = pitch_deg;
    m_roll_offset_deg  = roll_deg;
    rebuild_view();
}

// ---------------------------------------------------------------------------
// Camera::advance_frame
// ---------------------------------------------------------------------------
void Camera::advance_frame(uint32_t /*frame_index*/)
{
    m_prev_view = m_view;
    m_prev_proj = m_proj;
    // Jitter is handled in the shader via the Halton sequence (frame_index);
    // no CPU-side matrix modification needed for DLSS temporal accumulation.
}

// ---------------------------------------------------------------------------
// Camera::rebuild_view
// ---------------------------------------------------------------------------
void Camera::rebuild_view()
{
    Vec3 fwd   = m_forward;
    Vec3 right = m_right;
    Vec3 up    = m_up;

    if (std::abs(m_yaw_offset_deg) > k_epsilon)
    {
        float rad = m_yaw_offset_deg * k_deg2rad;
        float c   = std::cos(rad), s = std::sin(rad);
        Vec3 new_fwd = { fwd.x*c + right.x*s,
                         fwd.y*c + right.y*s,
                         fwd.z*c + right.z*s };
        right = { fwd.x*(-s) + right.x*c,
                  fwd.y*(-s) + right.y*c,
                  fwd.z*(-s) + right.z*c };
        fwd   = new_fwd;
    }
    if (std::abs(m_pitch_offset_deg) > k_epsilon)
    {
        float rad = m_pitch_offset_deg * k_deg2rad;
        float c   = std::cos(rad), s = std::sin(rad);
        Vec3 new_fwd = { fwd.x*c + up.x*s,
                         fwd.y*c + up.y*s,
                         fwd.z*c + up.z*s };
        up  = { fwd.x*(-s) + up.x*c,
                fwd.y*(-s) + up.y*c,
                fwd.z*(-s) + up.z*c };
        fwd = new_fwd;
    }

    // Row-major right-handed view matrix (Y-up):
    //  [ right.x  right.y  right.z  -dot(right, pos) ]
    //  [ up.x     up.y     up.z     -dot(up,    pos) ]
    //  [-fwd.x   -fwd.y   -fwd.z     dot(fwd,   pos) ]
    //  [ 0        0        0          1               ]
    m_view.m[0][0] =  right.x;  m_view.m[0][1] =  right.y;  m_view.m[0][2] =  right.z;
    m_view.m[0][3] = -(right.x*m_position.x + right.y*m_position.y + right.z*m_position.z);
    m_view.m[1][0] =  up.x;     m_view.m[1][1] =  up.y;     m_view.m[1][2] =  up.z;
    m_view.m[1][3] = -(up.x   *m_position.x + up.y   *m_position.y + up.z   *m_position.z);
    m_view.m[2][0] = -fwd.x;   m_view.m[2][1] = -fwd.y;   m_view.m[2][2] = -fwd.z;
    m_view.m[2][3] =  (fwd.x  *m_position.x + fwd.y  *m_position.y + fwd.z  *m_position.z);
    m_view.m[3][3] =  1.0f;

    m_view_inv = invert_rigid(m_view);
}

// ---------------------------------------------------------------------------
// Camera::rebuild_proj
// ---------------------------------------------------------------------------
void Camera::rebuild_proj()
{
    // Convert horizontal FOV to vertical FOV.
    float fov_h_rad = m_fov_h_deg * k_deg2rad;
    float fov_v_rad = 2.0f * std::atan(std::tan(fov_h_rad * 0.5f) / m_aspect);

    float f = 1.0f / std::tan(fov_v_rad * 0.5f);
    float n = m_near_z;
    float c = m_far_z / (m_far_z - n);
    float d = -n * m_far_z / (m_far_z - n);

    // D3D12 row-major perspective (depth [0, 1]):
    //  [ f/aspect  0   0  0 ]
    //  [ 0         f   0  0 ]
    //  [ 0         0   c  d ]
    //  [ 0         0   1  0 ]
    m_proj = {};
    m_proj.m[0][0] = f / m_aspect;
    m_proj.m[1][1] = f;
    m_proj.m[2][2] = c;
    m_proj.m[2][3] = d;
    m_proj.m[3][2] = 1.0f;

    m_proj_inv = invert_proj(m_proj);
}

// ---------------------------------------------------------------------------
// FlyCamera::init
// ---------------------------------------------------------------------------
void FlyCamera::init(const Vec3& position, float yaw_deg, float pitch_deg,
                     float fov_h_deg, float aspect, float near_z, float far_z)
{
    m_yaw_deg   = yaw_deg;
    m_pitch_deg = pitch_deg;
    m_fov_h_deg = fov_h_deg;
    m_near_z    = near_z;
    m_far_z     = far_z;

    float yr = yaw_deg   * k_deg2rad;
    float pr = pitch_deg * k_deg2rad;

    Vec3 fwd = {
        std::cos(pr) * std::sin(yr),
        std::sin(pr),
        std::cos(pr) * std::cos(yr)
    };
    fwd = fwd.normalized();

    m_camera.set_position(position);
    m_camera.set_look_at(position + fwd);
    m_camera.set_projection(fov_h_deg, aspect, near_z, far_z);
}

// ---------------------------------------------------------------------------
// FlyCamera::update
// ---------------------------------------------------------------------------
void FlyCamera::update(float dt, Vec3 move, float mouse_dx, float mouse_dy)
{
    m_yaw_deg   += mouse_dx * mouse_sens;
    m_pitch_deg += mouse_dy * mouse_sens;
    m_pitch_deg  = std::clamp(m_pitch_deg, -89.0f, 89.0f);

    float yr = m_yaw_deg   * k_deg2rad;
    float pr = m_pitch_deg * k_deg2rad;

    Vec3 fwd = {
        std::cos(pr) * std::sin(yr),
        std::sin(pr),
        std::cos(pr) * std::cos(yr)
    };
    fwd = fwd.normalized();

    Vec3 world_up = { 0.0f, 1.0f, 0.0f };
    Vec3 right    = fwd.cross(world_up).normalized();
    Vec3 up       = right.cross(fwd).normalized();

    Vec3 vel     = right * move.x + world_up * move.y + fwd * (-move.z);
    Vec3 new_pos = m_camera.position() + vel * (move_speed * dt);

    m_camera.set_position(new_pos);
    m_camera.set_look_at(new_pos + fwd, up);
}

// ---------------------------------------------------------------------------
// FlyCamera::set_aspect
// ---------------------------------------------------------------------------
void FlyCamera::set_aspect(float aspect)
{
    m_camera.set_projection(m_fov_h_deg, aspect, m_near_z, m_far_z);
}

} // namespace mars
