# =============================================================================
# mars_omm_baker — Offline Opacity Micro-Map (OMM) baking tool
#
# Evaluates the alpha channel of each vegetation species' base-color texture
# at micro-triangle resolution and outputs a pre-built OMM array in the
# D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE format as a binary
# blob (.ommblob).  The blob is referenced at runtime so DXR can skip
# per-any-hit alpha tests on opaque micro-triangles.
#
# Usage:
#   mars_omm_baker --model   <path/to/model.fbx>
#                  --texture <path/to/alpha.dds>
#                  --output  <path/to/output.ommblob>
#                 [--subdivision-level <N>]   default: 4 (256 micro-tris/tri)
#
# Requires NVIDIA DXR OMM extension support (RTX 30+ / DXR 1.2+).
# =============================================================================

file(GLOB_RECURSE OMM_BAKER_SOURCES "src/*.cpp")
file(GLOB_RECURSE OMM_BAKER_HEADERS "src/*.h" "src/*.hpp")

add_executable(mars_omm_baker
	${OMM_BAKER_SOURCES}
	${OMM_BAKER_HEADERS}
)

# -----------------------------------------------------------------------------
# Link against the engine library
# -----------------------------------------------------------------------------
target_link_libraries(mars_omm_baker
	PRIVATE
		MARS::Engine
)

# -----------------------------------------------------------------------------
# Compiler options (MSVC)
# -----------------------------------------------------------------------------
target_compile_options(mars_omm_baker
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
target_sources(mars_omm_baker PRIVATE
	"${CMAKE_SOURCE_DIR}/test_app/src/agility_sdk_exports.cpp"
)
mars_deploy_agility_sdk(mars_omm_baker)

# -----------------------------------------------------------------------------
# NVIDIA Streamline — deploy sl.*.dll and nvngx_*.dll
# -----------------------------------------------------------------------------
mars_deploy_streamline(mars_omm_baker)

# -----------------------------------------------------------------------------
# NVIDIA Nsight Aftermath — deploy GFSDK_Aftermath_Lib.x64.dll
# -----------------------------------------------------------------------------
mars_deploy_aftermath(mars_omm_baker)

# -----------------------------------------------------------------------------
# DXR Shaders — copy compiled DXIL files beside the executable
# -----------------------------------------------------------------------------
add_custom_command(TARGET mars_omm_baker POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mars_omm_baker>/dxil"
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${CMAKE_BINARY_DIR}/dxil"
		"$<TARGET_FILE_DIR:mars_omm_baker>/dxil"
	COMMENT "Copying DXIL shaders to omm_baker output directory..."
	VERBATIM
)
add_dependencies(mars_omm_baker shaders_dxil)

set_target_properties(mars_omm_baker PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
