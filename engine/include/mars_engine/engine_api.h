// =============================================================================
// engine_api.h
// MARS 3D Engine — DLL export / import macro
//
// When MARS_ENGINE_EXPORTS is defined (i.e. when building the DLL itself),
// MARS_ENGINE_API expands to __declspec(dllexport).
// When consumed by external projects (MARS_ENGINE_IMPORTS is defined instead),
// MARS_ENGINE_API expands to __declspec(dllimport).
// =============================================================================

#pragma once

#include <windows.h>
#include <format>
#include <string>

// mars_engine is a static library — no dllexport/dllimport needed.
// MARS_ENGINE_API is kept as a no-op placeholder so that existing and future
// declarations compile unchanged if the library is ever converted to a DLL.
#define MARS_ENGINE_API

// Route all engine log output to the VS Output (Debug) window via
// OutputDebugStringA.  No console window is required.
#ifndef MARS_LOG
#define MARS_LOG(...) do { std::string _s = std::format(__VA_ARGS__); _s += '\n'; OutputDebugStringA(_s.c_str()); } while(0)
#endif
