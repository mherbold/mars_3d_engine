// =============================================================================
// d3d12_agility.h
// MARS Engine — Windows Agility SDK version constants and include guard.
//
// Include this header INSTEAD of <d3d12.h> throughout the engine so that:
//   1. The Agility SDK's d3d12.h is always preferred over the Windows SDK's
//      older copy (include path order is controlled by CMake via AgilitySDK.cmake).
//   2. All d3dx12 helper headers are pulled in consistently.
//   3. The numeric version constant MARS_AGILITY_SDK_VERSION is available
//      for any compile-time version checks.
//
// The two exported symbols (D3D12SDKVersion / D3D12SDKPath) that tell the
// OS loader which Agility runtime DLL to use are defined in:
//   test_app/src/agility_sdk_exports.cpp
// That file must be compiled into every Win32 executable target.
// =============================================================================

#pragma once

// Pull in the Agility SDK d3d12.h (shadowed ahead of the Windows SDK copy).
#include <d3d12.h>
#include <d3d12sdklayers.h>

// d3dx12 helper library — convenience wrappers for barriers, root signatures,
// pipeline state streams, resource helpers, etc.
#include <d3dx12/d3dx12.h>

// DXGI headers (swap chain, adapter enumeration, HDR color space support)
#include <dxgi1_6.h>
#include <dxgidebug.h>

#ifndef MARS_AGILITY_SDK_VERSION
    // Fallback if CMake didn't inject the definition (e.g. in editor indexing).
    #define MARS_AGILITY_SDK_VERSION 614
#endif

// Convenience: the string form used by D3D12SDKPath (see agility_sdk_exports.cpp)
#define MARS_AGILITY_SDK_PATH_LITERAL ".\\D3D12\\"
