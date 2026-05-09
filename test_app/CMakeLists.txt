# =============================================================================
# mars_test_app — MARS 3D Engine Test Application (Win32 Executable)
#
# Loads a small demo scene and allows the user to fly around using
# a first-person free-camera, exercising the mars_engine library.
# =============================================================================

# -----------------------------------------------------------------------------
# Source and header discovery
# -----------------------------------------------------------------------------
file(GLOB_RECURSE TEST_APP_SOURCES
    "src/*.cpp"
)

file(GLOB_RECURSE TEST_APP_HEADERS
    "src/*.h"
    "src/*.hpp"
)

# -----------------------------------------------------------------------------
# Executable target
# WIN32 replaces the default console subsystem with the Windows GUI subsystem
# so the entry point is WinMain rather than main().
# -----------------------------------------------------------------------------
add_executable(mars_test_app WIN32
    ${TEST_APP_SOURCES}
    ${TEST_APP_HEADERS}
)

# -----------------------------------------------------------------------------
# Link against the engine library (and its transitive dependencies)
# -----------------------------------------------------------------------------
target_link_libraries(mars_test_app
    PRIVATE
        MARS::Engine  # Provides mars_engine.lib + public include paths
)

# -----------------------------------------------------------------------------
# Compile definitions
# -----------------------------------------------------------------------------
target_compile_definitions(mars_test_app
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
)

# -----------------------------------------------------------------------------
# Compiler options (MSVC)
# -----------------------------------------------------------------------------
target_compile_options(mars_test_app
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:
            /W4           # Enable high warning level
            /WX           # Treat warnings as errors
            /MP           # Multi-processor compilation
            /permissive-  # Strict ISO C++ conformance
        >
)

# -----------------------------------------------------------------------------
# Make mars_test_app the default startup project in Visual Studio
# -----------------------------------------------------------------------------
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT mars_test_app)
