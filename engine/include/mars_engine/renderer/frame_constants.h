// =============================================================================
// frame_constants.h
// MARS 3D Engine — Per-frame constant buffer layout (C++ side)
//
// Must match the FrameConstants cbuffer in path_trace.hlsl exactly.
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../math/math_types.h"

#include <cstdint>

namespace mars
{

// Matches FrameConstants cbuffer (b0, space0) in the shaders.
// Sized to an exact multiple of 256 bytes as required by D3D12 CBV alignment.
//
// HLSL cbuffer packing notes:
//   - Each 16-byte register holds up to 4 floats.
//   - A float3 crossing a 16-byte boundary is pushed to the next register.
//   - Two uint pads (_pad_align0/1) are inserted so sun_direction lands at offset 192 (register 12).
//
// C++ memory layout (512 bytes = 2 × 256):
//   offset   0 : view_inv (64)
//   offset  64 : proj_inv (64)
//   offset 128 : camera_pos (12) + frame_index (4)
//   offset 144 : output_width (4) + output_height (4) + tlas_slot (4) + material_buffer_slot (4)
//   offset 160 : instance_buffer_slot (4) + output_uav_slot (4) + motion_vector_uav_slot (4) + depth_uav_slot (4)
//   offset 176 : _pad_align0 (4) + _pad_align1 (4) + _pad_align2 (4) + _pad_align3 (4)  ← full register
//   offset 192 : sun_direction (12) + sun_intensity (4)
//   offset 208 : sun_color (12) + _pad0 (4)
//   offset 224 : prev_view_proj (64)
//   offset 288 : _pad[56] — 56 bytes padding to reach 512
//   total = 512 bytes
struct FrameConstants
{
    Mat4x4   view_inv;                              //  64 bytes  (offset   0)
    Mat4x4   proj_inv;                              //  64 bytes  (offset  64)
    Vec3     camera_pos;                            //  12 bytes  (offset 128)
    uint32_t frame_index = 0;                       //   4 bytes  (offset 140) → register boundary at 144

    uint32_t output_width  = 0;                     //   4 bytes  (offset 144)
    uint32_t output_height = 0;                     //   4 bytes  (offset 148)
    uint32_t tlas_slot     = UINT32_MAX;            //   4 bytes  (offset 152)
    uint32_t material_buffer_slot = UINT32_MAX;     //   4 bytes  (offset 156)

    uint32_t instance_buffer_slot     = UINT32_MAX; //   4 bytes  (offset 160)
    uint32_t output_uav_slot          = UINT32_MAX; //   4 bytes  (offset 164)
    uint32_t motion_vector_uav_slot   = UINT32_MAX; //   4 bytes  (offset 168)
    uint32_t depth_uav_slot           = UINT32_MAX; //   4 bytes  (offset 172)

    uint32_t _pad_align0              = 0;          //   4 bytes  (offset 176)
    uint32_t _pad_align1              = 0;          //   4 bytes  (offset 180)
    uint32_t _pad_align2              = 0;          //   4 bytes  (offset 184)
    uint32_t _pad_align3              = 0;          //   4 bytes  (offset 188)

    Vec3     sun_direction        = { 0.3f,  0.8f, 0.5f }; //  12 bytes  (offset 192)
    float    sun_intensity        = 5.0f;                   //   4 bytes  (offset 204)
    Vec3     sun_color            = { 1.0f, 0.95f, 0.85f }; // 12 bytes  (offset 208)
    float    _pad0                = 0.0f;                   //   4 bytes  (offset 220)

    // previous-frame view-projection matrix (for motion vector computation in the shader)
    Mat4x4   prev_view_proj;                        //  64 bytes  (offset 224)

    float    _pad[56]             = {};             //  56 bytes  (offset 288) — pad to 512
};

static_assert(sizeof(FrameConstants) % 256 == 0,
              "FrameConstants must be a multiple of 256 bytes for D3D12 CBV alignment");

} // namespace mars
