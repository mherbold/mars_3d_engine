# =============================================================================
# engine/shaders/CMakeLists.txt
# Compiles all HLSL shaders to DXIL using DXC.
#
# Defines a custom CMake target: shaders_dxil
# The engine library and test_app both depend on this target so that shaders
# are compiled before any C++ executable tries to load them at runtime.
# =============================================================================

include(CompileHLSL)

# ---------------------------------------------------------------------------
# Shader source files (must be listed explicitly for CMake dependency tracking)
# ---------------------------------------------------------------------------
set(HLSL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/path_trace.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shadow_ray.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/tone_map_blit_vs.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/tone_map_blit_ps.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/gi/restir_di.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/gi/restir_gi.hlsl"
)

# HLSLI headers — listed so they appear in the IDE; not compiled individually
set(HLSLI_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/common/bindless.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/math.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/random.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/material/pbr_brdf.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/material/material_data.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/sky/physical_sky.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/sky/hdri_sky.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/gi/restir_di.hlsli"
)

# ---------------------------------------------------------------------------
# Custom target: shaders_dxil
# ---------------------------------------------------------------------------
add_custom_target(shaders_dxil
    COMMENT "Compiling HLSL shaders to DXIL..."
    SOURCES ${HLSL_SOURCES} ${HLSLI_HEADERS}
)

# Tell the VS IDE not to auto-compile these with its built-in HLSL compiler.
# Our DXC-based custom commands (added by target_compile_hlsl) handle compilation.
set_source_files_properties(${HLSL_SOURCES} ${HLSLI_HEADERS}
    PROPERTIES
        VS_TOOL_OVERRIDE "None"
        HEADER_FILE_ONLY TRUE
)

# Compile HLSL → DXIL (adds add_custom_command entries and attaches outputs
# to the shaders_dxil target)
target_compile_hlsl(
    TARGET      shaders_dxil
    SOURCES     ${HLSL_SOURCES}
    OUTPUT_DIR  "${CMAKE_BINARY_DIR}/dxil"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

# ---------------------------------------------------------------------------
# Source groups for Visual Studio IDE organisation
# ---------------------------------------------------------------------------
source_group("Shaders"              FILES ${HLSL_SOURCES})
source_group("Shaders\\\\common"    FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/common/bindless.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/math.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/random.hlsli"
)
source_group("Shaders\\\\material"  FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/material/pbr_brdf.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/material/material_data.hlsli"
)
source_group("Shaders\\\\sky"       FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/sky/physical_sky.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/sky/hdri_sky.hlsli"
)
source_group("Shaders\\\\gi"        FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/gi/restir_di.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/gi/restir_di.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/gi/restir_gi.hlsl"
)
source_group("Shaders\\\\post"      FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/tone_map_blit_vs.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/tone_map_blit_ps.hlsl"
)
