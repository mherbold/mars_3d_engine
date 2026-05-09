# =============================================================================
# mars_engine — MARS Full Path Tracing 3D Engine (Windows Shared Library / DLL)
# =============================================================================

# -----------------------------------------------------------------------------
# Source and header discovery
# Using explicit globbing here; switch to explicit file lists as the project grows.
# -----------------------------------------------------------------------------
file(GLOB_RECURSE ENGINE_SOURCES
    "src/*.cpp"
)

file(GLOB_RECURSE ENGINE_HEADERS
    "include/*.h"
    "include/*.hpp"
)

# -----------------------------------------------------------------------------
# Library target
# SHARED produces a .dll + .lib import library on Windows.
# -----------------------------------------------------------------------------
add_library(mars_engine SHARED
    ${ENGINE_SOURCES}
    ${ENGINE_HEADERS}
)

# Alias so consumers can use the same target name regardless of build context.
add_library(MARS::Engine ALIAS mars_engine)

# -----------------------------------------------------------------------------
# Output name and versioning
# -----------------------------------------------------------------------------
set_target_properties(mars_engine PROPERTIES
    OUTPUT_NAME   "mars_engine"
    VERSION       ${PROJECT_VERSION}
    SOVERSION     ${PROJECT_VERSION_MAJOR}
    # Symbols are controlled manually via MARS_ENGINE_API macro (see engine_api.h)
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
)

# -----------------------------------------------------------------------------
# Include directories
#   PUBLIC  — consumers (test_app, etc.) inherit these paths automatically.
#   PRIVATE — internal implementation paths not exposed to consumers.
# -----------------------------------------------------------------------------
target_include_directories(mars_engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# -----------------------------------------------------------------------------
# Compile definitions
#   MARS_ENGINE_EXPORTS — present when building the DLL; enables dllexport.
#   MARS_ENGINE_IMPORTS — inherited by consumers; enables dllimport.
# -----------------------------------------------------------------------------
target_compile_definitions(mars_engine
    PRIVATE
        MARS_ENGINE_EXPORTS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    INTERFACE
        MARS_ENGINE_IMPORTS
)

# -----------------------------------------------------------------------------
# Compiler options (MSVC)
# -----------------------------------------------------------------------------
target_compile_options(mars_engine
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:
            /W4           # Enable high warning level
            /WX           # Treat warnings as errors
            /MP           # Multi-processor compilation
            /permissive-  # Strict ISO C++ conformance
        >
)

# -----------------------------------------------------------------------------
# Windows / DirectX 12 system libraries
#   d3d12      — Direct3D 12 (GPU commands, DXR ray tracing pipeline)
#   dxgi       — DXGI swap chain and adapter enumeration
#   dxguid     — GUIDs for DirectX interfaces
#   d3dcompiler — Runtime HLSL shader compilation
# -----------------------------------------------------------------------------
target_link_libraries(mars_engine
    PRIVATE
        d3d12.lib
        dxgi.lib
        dxguid.lib
        d3dcompiler.lib
)
