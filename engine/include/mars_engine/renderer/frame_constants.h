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
// Layout (bytes): 2×Mat4x4(128) + Vec3(12) + 7×uint32_t(28) + _pad[22](88) = 256
struct FrameConstants
{
    Mat4x4   view_inv;                         //  64 bytes
    Mat4x4   proj_inv;                         //  64 bytes
    Vec3     camera_pos;                       //  12 bytes
    uint32_t frame_index = 0;                  //   4 bytes

    uint32_t output_width  = 0;                //   4 bytes
    uint32_t output_height = 0;                //   4 bytes
    uint32_t tlas_slot     = UINT32_MAX;       //   4 bytes
    uint32_t material_buffer_slot = UINT32_MAX;//   4 bytes

    uint32_t instance_buffer_slot = UINT32_MAX;//   4 bytes
    uint32_t output_uav_slot      = UINT32_MAX;//   4 bytes
    float    _pad[22]             = {};        //  88 bytes
};

static_assert(sizeof(FrameConstants) % 256 == 0,
              "FrameConstants must be a multiple of 256 bytes for D3D12 CBV alignment");

} // namespace mars
