// =============================================================================
// mars_engine.h
// MARS 3D Engine — Top-level public header
//
// Include this single header to access the full public API of the engine.
// All sub-system headers are transitively included from here.
// =============================================================================

#pragma once

#include "engine_api.h"
#include "math/math_types.h"
#include "scene/scene_types.h"
#include "scene/scene.h"
#include "scene/scene_loader.h"
#include "camera/camera.h"
#include "renderer/device_context.h"
#include "renderer/display_output.h"
#include "renderer/display_manager.h"
#include "renderer/frame_constants.h"
#include "renderer/path_tracer.h"
#include "renderer/renderer.h"
#include "asset/asset_types.h"
#include "asset/asset_importer.h"
#include "asset/gpu_mesh_buffer.h"
#include "asset/texture_loader.h"
#include "asset/resource_manager.h"
