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
# Compile definitions are inherited from mars_engine (PUBLIC) — no extras needed here.

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

# -----------------------------------------------------------------------------
# Windows Agility SDK — deploy runtime DLLs beside the executable
#
# mars_configure_agility_sdk() was already called on mars_engine (which
# provides the headers and d3dx12 impl via PUBLIC linkage).
# mars_deploy_agility_sdk() must be called on the executable so that
# D3D12Core.dll / d3d12SDKLayers.dll land next to mars_test_app.exe.
# agility_sdk_exports.cpp exports D3D12SDKVersion + D3D12SDKPath — these
# MUST be in the executable, not in the static lib.
# -----------------------------------------------------------------------------
target_sources(mars_test_app PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/agility_sdk_exports.cpp"
)
mars_deploy_agility_sdk(mars_test_app)

# -----------------------------------------------------------------------------
# NVIDIA Streamline — deploy sl.*.dll and nvngx_*.dll beside the executable.
# The engine links sl.interposer.lib (via MARS::Streamline); the DLLs must
# be co-located with the exe for the runtime loader to find them.
# -----------------------------------------------------------------------------
mars_deploy_streamline(mars_test_app)

# -----------------------------------------------------------------------------
# NVIDIA Nsight Aftermath — deploy GFSDK_Aftermath_Lib.x64.dll beside the exe.
# -----------------------------------------------------------------------------
mars_deploy_aftermath(mars_test_app)

# -----------------------------------------------------------------------------
# DXR Shaders — copy compiled DXIL files into a dxil/ sub-directory
# next to the executable so PathTracer can load them at runtime.
# -----------------------------------------------------------------------------
add_custom_command(TARGET mars_test_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mars_test_app>/dxil"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_BINARY_DIR}/dxil"
        "$<TARGET_FILE_DIR:mars_test_app>/dxil"
    COMMENT "Copying DXIL shaders to output directory..."
    VERBATIM
)
add_dependencies(mars_test_app shaders_dxil)

# -----------------------------------------------------------------------------
# Scene assets — copy test_scene.marsscene and the models/ folder next to the
# executable so the app can find them regardless of working directory.
# The VS debugger working directory is also set to the source root so that
# F5 launches find the assets via either the CWD or the exe-relative path.
# -----------------------------------------------------------------------------
add_custom_command(TARGET mars_test_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/test_scene.marsscene"
        "$<TARGET_FILE_DIR:mars_test_app>/test_scene.marsscene"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/models"
        "$<TARGET_FILE_DIR:mars_test_app>/models"
    COMMENT "Copying scene file and models to output directory..."
    VERBATIM
)

set_target_properties(mars_test_app PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
