// =============================================================================
// skeleton.h
// MARS 3D Engine — Skeleton data structure for skeletal animation
//
// Defines the bone hierarchy and skeleton data structure for skinned meshes.
// Loaded from FBX/glTF via AssetImporter.
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
// Maximum number of bones per skeleton
// =============================================================================
constexpr uint32_t k_max_bones_per_skeleton = 256;

// =============================================================================
// Bone — a single bone in the skeleton hierarchy
// =============================================================================
struct Bone
{
	std::string name;
	int32_t     parent_index = -1; // -1 = root bone
	Mat4x4      inverse_bind_pose; // Transforms from model space to bone space
	Mat4x4      local_transform;   // Local transform relative to parent (bind pose)
};

// =============================================================================
// Skeleton — bone hierarchy for a skinned mesh
// =============================================================================
struct Skeleton
{
	std::string       name;
	std::vector<Bone> bones;

	// Find a bone index by name (returns -1 if not found)
	int32_t find_bone_index(const std::string& bone_name) const
	{
		for (size_t i = 0; i < bones.size(); ++i)
		{
			if (bones[i].name == bone_name)
				return static_cast<int32_t>(i);
		}
		return -1;
	}
};

} // namespace mars
