// =============================================================================
// math.hlsli
// MARS Common — math constants and utility functions.
// =============================================================================

#ifndef MARS_MATH_HLSLI
#define MARS_MATH_HLSLI

static const float PI      = 3.14159265358979323846f;
static const float TWO_PI  = 6.28318530717958647692f;
static const float INV_PI  = 0.31830988618379067154f;
static const float HALF_PI = 1.57079632679489661923f;

// Safe normalize (returns zero vector if length < epsilon)
float3 SafeNormalize(float3 v)
{
    float len = length(v);
    return (len > 1e-8f) ? (v / len) : float3(0, 0, 0);
}

// Luminance of a linear RGB colour (BT.709 primaries)
float Luminance(float3 rgb)
{
    return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

// Build an orthonormal basis from a single normal vector (Frisvad 2012)
void BuildONB(float3 n, out float3 tangent, out float3 bitangent)
{
    if (abs(n.x) > 0.9f)
        tangent = float3(0, 1, 0);
    else
        tangent = float3(1, 0, 0);
    bitangent = normalize(cross(n, tangent));
    tangent   = cross(bitangent, n);
}

// Transform a direction from local (ONB) space to world space
float3 LocalToWorld(float3 dir, float3 tangent, float3 bitangent, float3 normal)
{
    return dir.x * tangent + dir.y * bitangent + dir.z * normal;
}

#endif // MARS_MATH_HLSLI
