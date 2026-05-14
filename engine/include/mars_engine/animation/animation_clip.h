// =============================================================================
// animation_clip.h
// MARS 3D Engine — Animation clip data structure
//
// Defines keyframe animation data for skeletal animation.
// Supports position, rotation, and scale keys with linear interpolation.
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../math/math_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mars
{

// =============================================================================
// Keyframe data structures
// =============================================================================
struct PositionKey
{
	float time;
	Vec3  value;
};

struct RotationKey
{
	float      time;
	Quaternion value;
};

struct ScaleKey
{
	float time;
	float value; // Uniform scale only
};

// =============================================================================
// BoneChannel — all keyframes for a single bone
// =============================================================================
struct BoneChannel
{
	std::string                bone_name;
	std::vector<PositionKey>   position_keys;
	std::vector<RotationKey>   rotation_keys;
	std::vector<ScaleKey>      scale_keys;
};

// =============================================================================
// AnimationClip — a single animation sequence
// =============================================================================
struct AnimationClip
{
	std::string                name;
	float                      duration  = 0.0f; // In seconds
	float                      ticks_per_second = 30.0f; // Animation framerate
	std::vector<BoneChannel>   channels; // One per animated bone

	// Find a channel by bone name (returns nullptr if not found)
	const BoneChannel* find_channel(const std::string& bone_name) const
	{
		for (const auto& channel : channels)
		{
			if (channel.bone_name == bone_name)
				return &channel;
		}
		return nullptr;
	}
};

// =============================================================================
// Helper functions for keyframe interpolation
// =============================================================================

// Linear interpolation for position keys
inline Vec3 interpolate_position(const std::vector<PositionKey>& keys, float time)
{
	if (keys.empty()) return Vec3{};
	if (keys.size() == 1 || time <= keys.front().time) return keys.front().value;
	if (time >= keys.back().time) return keys.back().value;

	for (size_t i = 0; i < keys.size() - 1; ++i)
	{
		if (time >= keys[i].time && time < keys[i + 1].time)
		{
			float t = (time - keys[i].time) / (keys[i + 1].time - keys[i].time);
			return keys[i].value + (keys[i + 1].value - keys[i].value) * t;
		}
	}
	return keys.back().value;
}

// Spherical linear interpolation for rotation keys
inline Quaternion interpolate_rotation(const std::vector<RotationKey>& keys, float time)
{
	if (keys.empty()) return Quaternion::identity();
	if (keys.size() == 1 || time <= keys.front().time) return keys.front().value;
	if (time >= keys.back().time) return keys.back().value;

	for (size_t i = 0; i < keys.size() - 1; ++i)
	{
		if (time >= keys[i].time && time < keys[i + 1].time)
		{
			float t = (time - keys[i].time) / (keys[i + 1].time - keys[i].time);

			// Simplified SLERP (for now, use LERP + normalize)
			// TODO: Implement proper SLERP if animation quality requires it
			const Quaternion& q1 = keys[i].value;
			const Quaternion& q2 = keys[i + 1].value;

			float x = q1.x + (q2.x - q1.x) * t;
			float y = q1.y + (q2.y - q1.y) * t;
			float z = q1.z + (q2.z - q1.z) * t;
			float w = q1.w + (q2.w - q1.w) * t;

			// Normalize
			float len = std::sqrt(x*x + y*y + z*z + w*w);
			if (len > 0.0f)
			{
				float inv = 1.0f / len;
				return {x * inv, y * inv, z * inv, w * inv};
			}
			return Quaternion::identity();
		}
	}
	return keys.back().value;
}

// Linear interpolation for scale keys
inline float interpolate_scale(const std::vector<ScaleKey>& keys, float time)
{
	if (keys.empty()) return 1.0f;
	if (keys.size() == 1 || time <= keys.front().time) return keys.front().value;
	if (time >= keys.back().time) return keys.back().value;

	for (size_t i = 0; i < keys.size() - 1; ++i)
	{
		if (time >= keys[i].time && time < keys[i + 1].time)
		{
			float t = (time - keys[i].time) / (keys[i + 1].time - keys[i].time);
			return keys[i].value + (keys[i + 1].value - keys[i].value) * t;
		}
	}
	return keys.back().value;
}

} // namespace mars
