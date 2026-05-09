// =============================================================================
// math_types.h
// MARS 3D Engine — Core math types (vectors, matrices, quaternions, etc.)
// =============================================================================

#pragma once

#include "../engine_api.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mars
{

// =============================================================================
// Vec2
// =============================================================================
struct Vec2
{
    float x = 0.0f, y = 0.0f;

    Vec2() = default;
    constexpr Vec2(float x, float y) : x(x), y(y) {}

    Vec2  operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2  operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2  operator*(float s) const { return {x * s, y * s}; }
    float dot(Vec2 o)       const { return x * o.x + y * o.y; }
    float length()          const { return std::sqrt(dot(*this)); }
    Vec2  normalized()      const { float l = length(); return l > 0.0f ? (*this * (1.0f / l)) : Vec2{}; }
};

// =============================================================================
// Vec3
// =============================================================================
struct Vec3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3() = default;
    constexpr Vec3(float x, float y, float z) : x(x), y(y), z(z) {}

    Vec3  operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3  operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3  operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3  operator-()        const { return {-x, -y, -z}; }
    float dot(Vec3 o)       const { return x * o.x + y * o.y + z * o.z; }
    Vec3  cross(Vec3 o)     const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    float length()          const { return std::sqrt(dot(*this)); }
    Vec3  normalized()      const { float l = length(); return l > 0.0f ? (*this * (1.0f / l)) : Vec3{}; }
};

// =============================================================================
// Vec4
// =============================================================================
struct Vec4
{
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

    Vec4() = default;
    constexpr Vec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
    constexpr Vec4(Vec3 v, float w) : x(v.x), y(v.y), z(v.z), w(w) {}

    Vec4  operator+(Vec4 o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    Vec4  operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
    float dot(Vec4 o)       const { return x * o.x + y * o.y + z * o.z + w * o.w; }
};

// =============================================================================
// Mat4x4 — row-major, right-handed (matches HLSL row_major float4x4)
// =============================================================================
struct Mat4x4
{
    float m[4][4] = {};

    static Mat4x4 identity()
    {
        Mat4x4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }

    Mat4x4 operator*(const Mat4x4& o) const
    {
        Mat4x4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    r.m[i][j] += m[i][k] * o.m[k][j];
        return r;
    }

    Vec4 transform(Vec4 v) const
    {
        return {
            m[0][0]*v.x + m[0][1]*v.y + m[0][2]*v.z + m[0][3]*v.w,
            m[1][0]*v.x + m[1][1]*v.y + m[1][2]*v.z + m[1][3]*v.w,
            m[2][0]*v.x + m[2][1]*v.y + m[2][2]*v.z + m[2][3]*v.w,
            m[3][0]*v.x + m[3][1]*v.y + m[3][2]*v.z + m[3][3]*v.w,
        };
    }

    // Build a translation matrix.
    static Mat4x4 translation(Vec3 t)
    {
        Mat4x4 r = identity();
        r.m[0][3] = t.x;
        r.m[1][3] = t.y;
        r.m[2][3] = t.z;
        return r;
    }

    // Build a uniform-scale matrix.
    static Mat4x4 scale(float s)
    {
        Mat4x4 r = identity();
        r.m[0][0] = r.m[1][1] = r.m[2][2] = s;
        return r;
    }
};

// =============================================================================
// Quaternion
// =============================================================================
struct Quaternion
{
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

    Quaternion() = default;
    constexpr Quaternion(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}

    static Quaternion identity() { return {0.0f, 0.0f, 0.0f, 1.0f}; }

    Quaternion operator*(const Quaternion& o) const
    {
        return {
            w * o.x + x * o.w + y * o.z - z * o.y,
            w * o.y - x * o.z + y * o.w + z * o.x,
            w * o.z + x * o.y - y * o.x + z * o.w,
            w * o.w - x * o.x - y * o.y - z * o.z,
        };
    }

    // Convert to a 4x4 rotation matrix.
    Mat4x4 to_matrix() const
    {
        Mat4x4 r = Mat4x4::identity();
        r.m[0][0] = 1.0f - 2.0f*(y*y + z*z);
        r.m[0][1] = 2.0f*(x*y - z*w);
        r.m[0][2] = 2.0f*(x*z + y*w);
        r.m[1][0] = 2.0f*(x*y + z*w);
        r.m[1][1] = 1.0f - 2.0f*(x*x + z*z);
        r.m[1][2] = 2.0f*(y*z - x*w);
        r.m[2][0] = 2.0f*(x*z - y*w);
        r.m[2][1] = 2.0f*(y*z + x*w);
        r.m[2][2] = 1.0f - 2.0f*(x*x + y*y);
        return r;
    }
};

// =============================================================================
// Transform — position + rotation + uniform scale
// =============================================================================
struct Transform
{
    Vec3       position = {};
    Quaternion rotation = Quaternion::identity();
    float      scale    = 1.0f;

    Mat4x4 to_matrix() const
    {
        return Mat4x4::translation(position) * rotation.to_matrix() * Mat4x4::scale(scale);
    }
};

// =============================================================================
// AABB — axis-aligned bounding box
// =============================================================================
struct AABB
{
    Vec3 min_pt = {};
    Vec3 max_pt = {};

    Vec3 center()  const { return (min_pt + max_pt) * 0.5f; }
    Vec3 extents() const { return (max_pt - min_pt) * 0.5f; }

    void expand(Vec3 p)
    {
        min_pt.x = std::min(min_pt.x, p.x);
        min_pt.y = std::min(min_pt.y, p.y);
        min_pt.z = std::min(min_pt.z, p.z);
        max_pt.x = std::max(max_pt.x, p.x);
        max_pt.y = std::max(max_pt.y, p.y);
        max_pt.z = std::max(max_pt.z, p.z);
    }
};

} // namespace mars
