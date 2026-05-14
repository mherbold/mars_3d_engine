// =============================================================================
// animation_system.cpp
// MARS 3D Engine — Animation system implementation
// =============================================================================

#include "mars_engine/animation/animation_system.h"
#include "mars_engine/renderer/device_context.h"
#include "mars_engine/asset/resource_manager.h"

#include <algorithm>
#include <cassert>

namespace mars
{

// =============================================================================
// AnimationSystem
// =============================================================================

AnimationSystem::AnimationSystem() = default;
AnimationSystem::~AnimationSystem() = default;

void AnimationSystem::initialize(DeviceContext* device, ResourceManager* resources)
{
	m_device    = device;
	m_resources = resources;
	m_initialized = true;
}

void AnimationSystem::shutdown()
{
	m_skeletons.clear();
	m_clips.clear();
	m_states.clear();
	m_initialized = false;
}

uint32_t AnimationSystem::register_skeleton(const Skeleton& skeleton)
{
	assert(skeleton.bones.size() <= k_max_bones_per_skeleton && 
		   "Skeleton exceeds maximum bone count");

	m_skeletons.push_back(skeleton);
	return static_cast<uint32_t>(m_skeletons.size() - 1);
}

uint32_t AnimationSystem::register_animation_clip(const AnimationClip& clip)
{
	m_clips.push_back(clip);
	return static_cast<uint32_t>(m_clips.size() - 1);
}

uint32_t AnimationSystem::create_animation_state(uint32_t skeleton_id, uint32_t initial_clip_id)
{
	assert(skeleton_id < m_skeletons.size() && "Invalid skeleton ID");
	assert(initial_clip_id < m_clips.size() && "Invalid clip ID");

	AnimationState state;
	state.skeleton_id    = skeleton_id;
	state.active_clip_id = initial_clip_id;
	state.current_time   = 0.0f;
	state.looping        = true;
	state.playback_speed = 1.0f;

	m_states.push_back(state);
	return static_cast<uint32_t>(m_states.size() - 1);
}

void AnimationSystem::update(float delta_time)
{
	for (auto& state : m_states)
	{
		if (state.active_clip_id >= m_clips.size())
			continue;

		const AnimationClip& clip = m_clips[state.active_clip_id];

		// Advance animation time
		state.current_time += delta_time * state.playback_speed;

		// Handle looping
		if (state.looping)
		{
			while (state.current_time >= clip.duration)
				state.current_time -= clip.duration;
		}
		else
		{
			state.current_time = std::min(state.current_time, clip.duration);
		}

		// Update blend state if blending
		if (state.blend_weight > 0.0f && state.blend_duration > 0.0f)
		{
			state.blend_time += delta_time;
			state.blend_weight = std::min(1.0f, state.blend_time / state.blend_duration);

			// When blend is complete, switch to target clip
			if (state.blend_weight >= 1.0f)
			{
				state.active_clip_id = state.blend_target_clip_id;
				state.blend_weight   = 0.0f;
				state.blend_time     = 0.0f;
				state.current_time   = 0.0f;
			}
		}
	}
}

void AnimationSystem::evaluate_animation(uint32_t state_id, std::vector<Mat4x4>& out_bone_palette)
{
	assert(state_id < m_states.size() && "Invalid state ID");

	const AnimationState& state    = m_states[state_id];
	const Skeleton&       skeleton = m_skeletons[state.skeleton_id];
	const AnimationClip&  clip     = m_clips[state.active_clip_id];

	// Evaluate the active clip
	std::vector<Mat4x4> local_transforms;
	evaluate_clip(clip, skeleton, state.current_time, local_transforms);

	// If blending, evaluate the blend target and blend
	if (state.blend_weight > 0.0f && state.blend_target_clip_id < m_clips.size())
	{
		const AnimationClip& blend_clip = m_clips[state.blend_target_clip_id];
		std::vector<Mat4x4> blend_transforms;
		evaluate_clip(blend_clip, skeleton, 0.0f, blend_transforms);

		blend_bone_transforms(local_transforms, blend_transforms, state.blend_weight, local_transforms);
	}

	// Compute final bone palette (local → world → skinning space)
	compute_bone_palette(skeleton, local_transforms, out_bone_palette);
}

void AnimationSystem::start_blend(uint32_t state_id, uint32_t target_clip_id, float blend_duration)
{
	assert(state_id < m_states.size() && "Invalid state ID");
	assert(target_clip_id < m_clips.size() && "Invalid target clip ID");

	AnimationState& state = m_states[state_id];
	state.blend_target_clip_id = target_clip_id;
	state.blend_duration       = blend_duration;
	state.blend_time           = 0.0f;
	state.blend_weight         = (blend_duration > 0.0f) ? 0.0001f : 1.0f;
}

void AnimationSystem::set_playback_speed(uint32_t state_id, float speed)
{
	assert(state_id < m_states.size() && "Invalid state ID");
	m_states[state_id].playback_speed = speed;
}

void AnimationSystem::set_looping(uint32_t state_id, bool looping)
{
	assert(state_id < m_states.size() && "Invalid state ID");
	m_states[state_id].looping = looping;
}

const Skeleton* AnimationSystem::get_skeleton(uint32_t skeleton_id) const
{
	return (skeleton_id < m_skeletons.size()) ? &m_skeletons[skeleton_id] : nullptr;
}

const AnimationClip* AnimationSystem::get_clip(uint32_t clip_id) const
{
	return (clip_id < m_clips.size()) ? &m_clips[clip_id] : nullptr;
}

AnimationState* AnimationSystem::get_state(uint32_t state_id)
{
	return (state_id < m_states.size()) ? &m_states[state_id] : nullptr;
}

// =============================================================================
// Private helpers
// =============================================================================

void AnimationSystem::evaluate_clip(const AnimationClip& clip, const Skeleton& skeleton,
									 float time, std::vector<Mat4x4>& out_local_transforms)
{
	out_local_transforms.resize(skeleton.bones.size());

	// Initialize with bind pose local transforms
	for (size_t i = 0; i < skeleton.bones.size(); ++i)
	{
		out_local_transforms[i] = skeleton.bones[i].local_transform;
	}

	// Apply animation channels
	for (const auto& channel : clip.channels)
	{
		int32_t bone_index = skeleton.find_bone_index(channel.bone_name);
		if (bone_index < 0)
			continue;

		// Sample keyframes
		Vec3       pos   = interpolate_position(channel.position_keys, time);
		Quaternion rot   = interpolate_rotation(channel.rotation_keys, time);
		float      scale = interpolate_scale(channel.scale_keys, time);

		// Build local transform
		Transform transform;
		transform.position = pos;
		transform.rotation = rot;
		transform.scale    = scale;

		out_local_transforms[bone_index] = transform.to_matrix();
	}
}

void AnimationSystem::blend_bone_transforms(const std::vector<Mat4x4>& a,
											 const std::vector<Mat4x4>& b,
											 float blend_weight,
											 std::vector<Mat4x4>& out)
{
	// Simple matrix lerp (not ideal but works for basic cross-fading)
	// TODO: Implement proper decompose → SLERP → recompose if quality issues arise
	size_t count = std::min(a.size(), b.size());
	out.resize(count);

	for (size_t i = 0; i < count; ++i)
	{
		for (int row = 0; row < 4; ++row)
		{
			for (int col = 0; col < 4; ++col)
			{
				out[i].m[row][col] = a[i].m[row][col] * (1.0f - blend_weight)
								   + b[i].m[row][col] * blend_weight;
			}
		}
	}
}

void AnimationSystem::compute_bone_palette(const Skeleton& skeleton,
											const std::vector<Mat4x4>& local_transforms,
											std::vector<Mat4x4>& out_bone_palette)
{
	out_bone_palette.resize(skeleton.bones.size());

	// Convert local transforms to world space
	std::vector<Mat4x4> world_transforms(skeleton.bones.size());

	for (size_t i = 0; i < skeleton.bones.size(); ++i)
	{
		const Bone& bone = skeleton.bones[i];

		if (bone.parent_index < 0)
		{
			// Root bone: world = local
			world_transforms[i] = local_transforms[i];
		}
		else
		{
			// Child bone: world = parent_world * local
			world_transforms[i] = world_transforms[bone.parent_index] * local_transforms[i];
		}

		// Final skinning matrix: world * inverse_bind_pose
		out_bone_palette[i] = world_transforms[i] * bone.inverse_bind_pose;
	}
}

} // namespace mars
