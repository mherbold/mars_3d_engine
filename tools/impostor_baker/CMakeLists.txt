# =============================================================================
# mars_impostor_baker — Offline impostor atlas baking tool
#
# Renders N views of each vegetation species mesh onto an octahedral atlas
# texture using D3D12 rasterization and saves the result as a DDS file.
# Run this tool when adding new species or changing species geometry to
# regenerate the impostor atlases used by the engine's LOD3 impostor BLAS.
#
# Usage:
#   mars_impostor_baker --scene <path/to/scene.marsscene> [--output-dir <dir>]
#                       [--views <N>] [--atlas-size <px>]
# =============================================================================

include(CompileHLSL)

# ---------------------------------------------------------------------------
# Compile the VS + PS shaders used by the baker
# ---------------------------------------------------------------------------
set(IMPOSTOR_BAKER_HLSL_SOURCES
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/impostor_bake_vs.hlsl"
	"${CMAKE_CURRENT_SOURCE_DIR}/shaders/impostor_bake_ps.hlsl"
)

add_custom_target(impostor_baker_shaders_dxil DEPENDS ${IMPOSTOR_BAKER_HLSL_SOURCES})

target_compile_hlsl(
	TARGET     impostor_baker_shaders_dxil
	SOURCES    ${IMPOSTOR_BAKER_HLSL_SOURCES}
	OUTPUT_DIR "${CMAKE_BINARY_DIR}/dxil"
)

file(GLOB_RECURSE IMPOSTOR_BAKER_SOURCES "src/*.cpp")
file(GLOB_RECURSE IMPOSTOR_BAKER_HEADERS "src/*.h" "src/*.hpp")

add_executable(mars_impostor_baker
	${IMPOSTOR_BAKER_SOURCES}
	${IMPOSTOR_BAKER_HEADERS}
)

# -----------------------------------------------------------------------------
# Link against the engine library
# -----------------------------------------------------------------------------
target_link_libraries(mars_impostor_baker
	PRIVATE
		MARS::Engine
)

# -----------------------------------------------------------------------------
# Compiler options (MSVC)
# -----------------------------------------------------------------------------
target_compile_options(mars_impostor_baker
	PRIVATE
		$<$<CXX_COMPILER_ID:MSVC>:
			/W4
			/WX
			/MP
			/permissive-
		>
)

# -----------------------------------------------------------------------------
# Windows Agility SDK — deploy runtime DLLs beside the executable
# -----------------------------------------------------------------------------
target_sources(mars_impostor_baker PRIVATE
	"${CMAKE_SOURCE_DIR}/test_app/src/agility_sdk_exports.cpp"
)
mars_deploy_agility_sdk(mars_impostor_baker)

# -----------------------------------------------------------------------------
# NVIDIA Streamline — deploy sl.*.dll and nvngx_*.dll
# -----------------------------------------------------------------------------
mars_deploy_streamline(mars_impostor_baker)

# -----------------------------------------------------------------------------
# NVIDIA Nsight Aftermath — deploy GFSDK_Aftermath_Lib.x64.dll
# -----------------------------------------------------------------------------
mars_deploy_aftermath(mars_impostor_baker)

# -----------------------------------------------------------------------------
# DXR Shaders — copy compiled DXIL files beside the executable
# -----------------------------------------------------------------------------
add_custom_command(TARGET mars_impostor_baker POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mars_impostor_baker>/dxil"
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${CMAKE_BINARY_DIR}/dxil"
		"$<TARGET_FILE_DIR:mars_impostor_baker>/dxil"
	COMMENT "Copying DXIL shaders to impostor_baker output directory..."
	VERBATIM
)
add_dependencies(mars_impostor_baker shaders_dxil impostor_baker_shaders_dxil)

set_target_properties(mars_impostor_baker PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
