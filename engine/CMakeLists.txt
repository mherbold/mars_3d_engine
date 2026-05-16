# =============================================================================
# mars_engine — MARS Full Path Tracing 3D Engine (Static Library)
# =============================================================================

# -----------------------------------------------------------------------------
# Source and header discovery
# -----------------------------------------------------------------------------
file(GLOB_RECURSE ENGINE_SOURCES
    "src/*.cpp"
)

file(GLOB_RECURSE ENGINE_HEADERS
    "include/*.h"
    "include/*.hpp"
)

# Explicit list ensures new files are included even before the next GLOB re-run.
list(APPEND ENGINE_SOURCES
    "src/renderer/device_context.cpp"
    "src/renderer/display_output.cpp"
    "src/renderer/display_manager.cpp"
    "src/renderer/path_tracer.cpp"
    "src/renderer/denoiser.cpp"
    "src/renderer/renderer.cpp"
    "src/asset/asset_importer.cpp"
    "src/asset/exr_loader.cpp"
    "src/asset/miniz.c"
    "src/asset/gpu_mesh_buffer.cpp"
    "src/asset/texture_loader.cpp"
    "src/asset/resource_manager.cpp"
    "src/scene/scene.cpp"
    "src/scene/scene_loader.cpp"
    "src/camera/camera.cpp"
    "src/animation/animation_system.cpp"
)
list(REMOVE_DUPLICATES ENGINE_SOURCES)

# Third-party source files are compiled with warnings suppressed (/W0 /WX-)
# so that upstream code issues don't fail our strict /WX build.
set_source_files_properties(
    "src/asset/exr_loader.cpp"
    "src/asset/miniz.c"
    PROPERTIES COMPILE_OPTIONS "/W0;/WX-"
)

list(APPEND ENGINE_HEADERS
    "include/mars_engine/renderer/device_context.h"
    "include/mars_engine/renderer/display_output.h"
    "include/mars_engine/renderer/display_manager.h"
    "include/mars_engine/renderer/frame_constants.h"
    "include/mars_engine/renderer/path_tracer.h"
    "include/mars_engine/renderer/denoiser.h"
    "include/mars_engine/renderer/renderer.h"
    "include/mars_engine/asset/asset_types.h"
    "include/mars_engine/asset/asset_importer.h"
    "include/mars_engine/asset/gpu_mesh_buffer.h"
    "include/mars_engine/asset/texture_loader.h"
    "include/mars_engine/asset/resource_manager.h"
    "include/mars_engine/scene/scene_types.h"
    "include/mars_engine/scene/scene_loader.h"
    "include/mars_engine/scene/scene.h"
    "include/mars_engine/camera/camera.h"
    "include/mars_engine/animation/skeleton.h"
    "include/mars_engine/animation/animation_clip.h"
    "include/mars_engine/animation/animation_system.h"
)
list(REMOVE_DUPLICATES ENGINE_HEADERS)

# -----------------------------------------------------------------------------
# Library target
# STATIC produces a .lib that the test_app links directly.
# -----------------------------------------------------------------------------
add_library(mars_engine STATIC
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
        ${d3d12memoryallocator_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/third_party
)

# -----------------------------------------------------------------------------
# Compile definitions
#   MARS_ENGINE_EXPORTS — present when building the DLL; enables dllexport.
#   MARS_ENGINE_IMPORTS — inherited by consumers; enables dllimport.
# -----------------------------------------------------------------------------
target_compile_definitions(mars_engine
    PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
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
# Windows Agility SDK — Agility headers shadow the Windows SDK d3d12.h;
# d3dx12_property_format_table.cpp is added to engine sources here.
# -----------------------------------------------------------------------------
mars_configure_agility_sdk(mars_engine)

# -----------------------------------------------------------------------------
# Shader compilation target (HLSL -> DXIL via DXC)
# -----------------------------------------------------------------------------
add_subdirectory(shaders)
add_dependencies(mars_engine shaders_dxil)

# -----------------------------------------------------------------------------
# Windows / DirectX 12 system libraries
#   d3d12      — Direct3D 12 (GPU commands, DXR ray tracing pipeline)
#   dxgi       — DXGI swap chain and adapter enumeration
#   dxguid     — GUIDs for DirectX interfaces
#   d3dcompiler — Runtime HLSL shader compilation
# -----------------------------------------------------------------------------
target_link_libraries(mars_engine
    PUBLIC
        d3d12.lib
        dxgi.lib
        dxguid.lib
        d3dcompiler.lib
        assimp::assimp
        nlohmann_json::nlohmann_json
        D3D12MemoryAllocator
        harfbuzz
        freetype
        MARS::DirectXTK12
        MARS::DirectXTex
        MARS::DirectXMesh
        MARS::Streamline
)

# NVIDIA Nsight Aftermath — GPU crash-dump capture; treated as dev/debug tool
if(AFTERMATH_SDK_FOUND)
    target_link_libraries(mars_engine PRIVATE MARS::NsightAftermath)
endif()

# MARS_ENABLE_DEV_UI
if(MARS_ENABLE_DEV_UI)
    target_compile_definitions(mars_engine PUBLIC MARS_ENABLE_DEV_UI)
    # ImGui is header-only from FetchContent; create an interface target
    add_library(imgui_impl STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
    )
    target_include_directories(imgui_impl
        PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui_impl PUBLIC d3d12.lib dxgi.lib)
    target_link_libraries(mars_engine PUBLIC imgui_impl)
endif()
