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

// mars_engine is a static library — no dllexport/dllimport needed.
// MARS_ENGINE_API is kept as a no-op placeholder so that existing and future
// declarations compile unchanged if the library is ever converted to a DLL.
#define MARS_ENGINE_API
