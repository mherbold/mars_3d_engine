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

#if defined(MARS_ENGINE_EXPORTS)
    #define MARS_ENGINE_API __declspec(dllexport)
#elif defined(MARS_ENGINE_IMPORTS)
    #define MARS_ENGINE_API __declspec(dllimport)
#else
    #define MARS_ENGINE_API
#endif
