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
// Layout (bytes): 2×Mat4x4(128) + Vec3(12) + 7×uint32_t(28) + 2×uint32_t pad(8) + sun(32) + _pad[12](48) = 256
//
// NOTE: The 2-uint pad between output_uav_slot and sun_direction is required because HLSL cbuffer
// packing rules push a float3 to the next 16-byte register when fewer than 3 component slots remain
// in the current register.  Without these 8 bytes the sun_direction/sun_color fields are misread,
// producing an all-red scene.
struct FrameConstants
{
    Mat4x4   view_inv;                         //  64 bytes  (offset   0)
    Mat4x4   proj_inv;                         //  64 bytes  (offset  64)
    Vec3     camera_pos;                       //  12 bytes  (offset 128)
    uint32_t frame_index = 0;                  //   4 bytes  (offset 140)

    uint32_t output_width  = 0;                //   4 bytes  (offset 144)
    uint32_t output_height = 0;                //   4 bytes  (offset 148)
    uint32_t tlas_slot     = UINT32_MAX;       //   4 bytes  (offset 152)
    uint32_t material_buffer_slot = UINT32_MAX;//   4 bytes  (offset 156)

    uint32_t instance_buffer_slot = UINT32_MAX;//   4 bytes  (offset 160)
    uint32_t output_uav_slot      = UINT32_MAX;//   4 bytes  (offset 164)
    uint32_t _pad_align0          = 0;         //   4 bytes  (offset 168) — \_ align sun_direction
    uint32_t _pad_align1          = 0;         //   4 bytes  (offset 172) — /  to offset 176

    Vec3     sun_direction        = { 0.3f,  0.8f, 0.5f }; //  12 bytes  (offset 176)
    float    sun_intensity        = 5.0f;                   //   4 bytes  (offset 188)
    Vec3     sun_color            = { 1.0f, 0.95f, 0.85f }; // 12 bytes  (offset 192)
    float    _pad0                = 0.0f;                   //   4 bytes  (offset 204)

    float    _pad[12]             = {};        //  48 bytes  (offset 208)
};

static_assert(sizeof(FrameConstants) % 256 == 0,
              "FrameConstants must be a multiple of 256 bytes for D3D12 CBV alignment");

} // namespace mars
