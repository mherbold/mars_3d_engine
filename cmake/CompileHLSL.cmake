# =============================================================================
# CompileHLSL.cmake
# Helper function: compile one HLSL source file to DXIL via DXC.
#
# Usage:
#   target_compile_hlsl(
#       TARGET          shaders_dxil          # CMake target that DXIL files attach to
#       SOURCES         foo.hlsl bar.hlsl     # list of HLSL source files (absolute or relative to caller)
#       OUTPUT_DIR      ${CMAKE_BINARY_DIR}/dxil   # where .dxil files are written
#       INCLUDE_DIRS    ${shader_include_dirs}     # optional additional -I paths
#   )
#
# Requirements:
#   - DXC must be on PATH, or DXC_PATH cache variable set to the full path of dxc.exe
#   - Each HLSL file must contain a "profile" hint comment on its first line, e.g.:
#       // #profile lib_6_8
#     Supported profiles: lib_6_8 (default), cs_6_8, vs_6_8, ps_6_8, etc.
#     If no hint is found, lib_6_8 is used (suitable for DXR ray tracing libraries).
#
# Output:
#   Each foo.hlsl produces ${OUTPUT_DIR}/foo.dxil.
#   A CMake INTERFACE library target `mars_shader_headers` is populated with
#   the list of DXIL files as INTERFACE_SOURCES so that downstream targets
#   can express a dependency on the compiled shaders.
# =============================================================================

cmake_minimum_required(VERSION 3.28)

# Locate DXC
find_program(DXC_EXECUTABLE
    NAMES dxc dxc.exe
    HINTS
        "${DXC_PATH}"
        "$ENV{DXC_PATH}"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
    DOC "DirectX Shader Compiler (dxc.exe)"
)

if(NOT DXC_EXECUTABLE)
    message(WARNING
        "[CompileHLSL] dxc.exe not found. "
        "Set DXC_PATH to the directory containing dxc.exe, or add it to PATH. "
        "Shader compilation will be skipped until DXC is available."
    )
endif()

# ---------------------------------------------------------------------------
# target_compile_hlsl()
# ---------------------------------------------------------------------------
function(target_compile_hlsl)
    cmake_parse_arguments(ARG
        ""
        "TARGET;OUTPUT_DIR"
        "SOURCES;INCLUDE_DIRS"
        ${ARGN}
    )

    if(NOT DXC_EXECUTABLE)
        message(STATUS "[CompileHLSL] Skipping shader compilation (dxc not found).")
        return()
    endif()

    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_BINARY_DIR}/dxil")
    endif()

    file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}")

    # Build -I flags
    set(include_flags "")
    foreach(inc_dir IN LISTS ARG_INCLUDE_DIRS)
        list(APPEND include_flags "-I" "${inc_dir}")
    endforeach()

    set(all_dxil_outputs "")

    foreach(hlsl_src IN LISTS ARG_SOURCES)
        # Resolve to absolute path
        if(NOT IS_ABSOLUTE "${hlsl_src}")
            set(hlsl_src "${CMAKE_CURRENT_SOURCE_DIR}/${hlsl_src}")
        endif()

        get_filename_component(base_name "${hlsl_src}" NAME_WE)
        set(dxil_out "${ARG_OUTPUT_DIR}/${base_name}.dxil")

        # Read the first line to detect an optional profile hint comment
        file(STRINGS "${hlsl_src}" first_line LIMIT_COUNT 1)
        if(first_line MATCHES "#profile ([a-z0-9_]+)")
            set(profile "${CMAKE_MATCH_1}")
        else()
            set(profile "lib_6_8")   # default: DXR ray-tracing library
        endif()

        # Library shader profiles (lib_*) have no single entry point.
        if(profile MATCHES "^lib_")
            set(entry_flag "")
        else()
            set(entry_flag "-E;main")
        endif()

        add_custom_command(
            OUTPUT  "${dxil_out}"
            COMMAND "${DXC_EXECUTABLE}"
                    -T "${profile}"
                    ${entry_flag}
                    -Fo "${dxil_out}"
                    -nologo
                    -WX              # warnings as errors
                    -Zpc             # pack matrices in column-major order
                    ${include_flags}
                    "${hlsl_src}"
            DEPENDS "${hlsl_src}"
            COMMENT "DXC: ${base_name}.hlsl -> ${base_name}.dxil [${profile}]"
            VERBATIM
        )

        list(APPEND all_dxil_outputs "${dxil_out}")
    endforeach()

    # Attach DXIL outputs to the caller's target
    target_sources(${ARG_TARGET} PRIVATE ${all_dxil_outputs})
    set_source_files_properties(${all_dxil_outputs} PROPERTIES GENERATED TRUE)
endfunction()
