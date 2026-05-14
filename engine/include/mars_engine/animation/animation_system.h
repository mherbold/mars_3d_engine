// =============================================================================
// animation_system.h
// MARS 3D Engine — Animation system for skeletal and rigid-body animations
//
// Public API for the animation system. Manages skeletal animation clips,
// blend trees, and per-instance animation state. Evaluates animations on the
// CPU each frame and outputs bone palettes for GPU skinning.
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../math/math_types.h"
#include "skeleton.h"
#include "animation_clip.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mars
{

// Forward declarations
class DeviceContext;
class ResourceManager;

// =============================================================================
// AnimationState — per-instance animation state
// =============================================================================
struct AnimationState
{
	uint32_t skeleton_id       = 0;
	uint32_t active_clip_id    = 0;
	float    current_time      = 0.0f;
	bool     looping           = true;
	float    playback_speed    = 1.0f;

	// Blend state (for cross-fading between clips)
	uint32_t blend_target_clip_id = 0;
	float    blend_weight         = 0.0f; // 0.0 = active_clip, 1.0 = blend_target
	float    blend_duration       = 0.0f;
	float    blend_time           = 0.0f;
};

// =============================================================================
// AnimationSystem — manages all animation data and evaluation
// =============================================================================
class MARS_ENGINE_API AnimationSystem
{
public:
	AnimationSystem();
	~AnimationSystem();

	// Initialize the animation system
	void initialize(DeviceContext* device, ResourceManager* resources);
	void shutdown();

	// Register a skeleton (returns skeleton ID)
	uint32_t register_skeleton(const Skeleton& skeleton);

	// Register an animation clip (returns clip ID)
	uint32_t register_animation_clip(const AnimationClip& clip);

	// Create a new animation state for an instance (returns state ID)
	uint32_t create_animation_state(uint32_t skeleton_id, uint32_t initial_clip_id);

	// Update all animation states (call once per frame)
	void update(float delta_time);

	// Evaluate a specific animation state and return the bone palette
	// The bone palette is a flat array of 4x4 matrices (one per bone)
	void evaluate_animation(uint32_t state_id, std::vector<Mat4x4>& out_bone_palette);

	// Blend tree / cross-fade control
	void start_blend(uint32_t state_id, uint32_t target_clip_id, float blend_duration);
	void set_playback_speed(uint32_t state_id, float speed);
	void set_looping(uint32_t state_id, bool looping);

	// Query methods
	const Skeleton*      get_skeleton(uint32_t skeleton_id) const;
	const AnimationClip* get_clip(uint32_t clip_id) const;
	AnimationState*      get_state(uint32_t state_id);

private:
	void evaluate_clip(const AnimationClip& clip, const Skeleton& skeleton, float time,
					   std::vector<Mat4x4>& out_local_transforms);

	void blend_bone_transforms(const std::vector<Mat4x4>& a, const std::vector<Mat4x4>& b,
								float blend_weight, std::vector<Mat4x4>& out);

	void compute_bone_palette(const Skeleton& skeleton,
							  const std::vector<Mat4x4>& local_transforms,
							  std::vector<Mat4x4>& out_bone_palette);

private:
	DeviceContext*   m_device    = nullptr;
	ResourceManager* m_resources = nullptr;

	std::vector<Skeleton>       m_skeletons;
	std::vector<AnimationClip>  m_clips;
	std::vector<AnimationState> m_states;

	bool m_initialized = false;
};

} // namespace mars
