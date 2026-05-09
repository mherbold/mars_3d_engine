# =============================================================================
# AgilitySDK.cmake
# Locates the Windows Agility SDK (Microsoft.Direct3D.D3D12 NuGet package)
# and provides a helper function to wire it into engine and executable targets.
#
# Exposes:
#   AGILITY_SDK_DIR         — cache variable: root of build\native\ inside the package
#   AGILITY_SDK_VERSION     — numeric SDK version (e.g. 614)
#   AGILITY_SDK_FOUND       — TRUE when the SDK is usable
#   mars_configure_agility_sdk(target)
#       — adds include dirs and the d3dx12 helper .cpp to <target>
#   mars_deploy_agility_sdk(exe_target)
#       — adds a post-build step that copies D3D12Core.dll + d3d12SDKLayers.dll
#         into a D3D12/ subdirectory beside the built executable (required by
#         the Agility SDK loader)
# =============================================================================

cmake_minimum_required(VERSION 3.28)

# ---------------------------------------------------------------------------
# Default search path — the NuGet layout used by M0's nuget install command
# ---------------------------------------------------------------------------
set(AGILITY_SDK_DIR
    "C:/mars_deps/nuget/Microsoft.Direct3D.D3D12.1.614.1/build/native"
    CACHE PATH
    "Root of the Agility SDK build/native directory \
(contains include/, bin/x64/, src/d3dx12/)."
)

set(AGILITY_SDK_VERSION 614 CACHE STRING
    "Numeric Agility SDK version passed to D3D12SDKVersion export (e.g. 614 for 1.614.x).")

# ---------------------------------------------------------------------------
# Validate the SDK layout
# ---------------------------------------------------------------------------
if(EXISTS "${AGILITY_SDK_DIR}/include/d3d12.h" AND
   EXISTS "${AGILITY_SDK_DIR}/bin/x64/D3D12Core.dll")
    set(AGILITY_SDK_FOUND TRUE)
    message(STATUS "[AgilitySDK] Found at ${AGILITY_SDK_DIR} (version ${AGILITY_SDK_VERSION})")
else()
    set(AGILITY_SDK_FOUND FALSE)
    message(WARNING
        "[AgilitySDK] SDK not found at AGILITY_SDK_DIR='${AGILITY_SDK_DIR}'. "
        "Set AGILITY_SDK_DIR to the build/native folder inside the NuGet package. "
        "D3D12 headers will fall back to the Windows SDK headers."
    )
endif()

# ---------------------------------------------------------------------------
# mars_configure_agility_sdk(<target>)
#
# Adds to <target>:
#   - Agility SDK include directory (shadowing the Windows SDK d3d12.h)
#   - d3dx12_property_format_table.cpp (required by d3dx12 helper headers)
#   - MARS_AGILITY_SDK_VERSION compile definition
# ---------------------------------------------------------------------------
function(mars_configure_agility_sdk target)
    if(NOT AGILITY_SDK_FOUND)
        message(STATUS "[AgilitySDK] Skipping configuration for '${target}' (SDK not found).")
        return()
    endif()

    # Agility headers must come BEFORE the Windows SDK include paths so that
    # the newer d3d12.h shadows the one shipped with the OS.
    target_include_directories(${target}
        PUBLIC
            "${AGILITY_SDK_DIR}/include"
            "${AGILITY_SDK_DIR}/include/d3dx12"
    )

    # d3dx12_property_format_table.cpp is a required implementation file for
    # the d3dx12 helper library; it must be compiled into exactly one target.
    target_sources(${target} PRIVATE
        "${AGILITY_SDK_DIR}/src/d3dx12/d3dx12_property_format_table.cpp"
    )

    target_compile_definitions(${target}
        PUBLIC
            MARS_AGILITY_SDK_VERSION=${AGILITY_SDK_VERSION}
    )
endfunction()

# ---------------------------------------------------------------------------
# mars_deploy_agility_sdk(<exe_target>)
#
# Adds a post-build command that copies the two Agility runtime DLLs into
# a D3D12/ subdirectory beside the built executable.
#
# The Agility SDK loader looks for:
#   <exe_dir>/D3D12/D3D12Core.dll
#   <exe_dir>/D3D12/d3d12SDKLayers.dll   (debug validation; optional at runtime)
#
# This must be called on the WIN32 executable target (mars_test_app), not
# on the static engine library.
# ---------------------------------------------------------------------------
function(mars_deploy_agility_sdk exe_target)
    if(NOT AGILITY_SDK_FOUND)
        message(STATUS "[AgilitySDK] Skipping deploy for '${exe_target}' (SDK not found).")
        return()
    endif()

    set(src_dir "${AGILITY_SDK_DIR}/bin/x64")
    set(dst_expr "$<TARGET_FILE_DIR:${exe_target}>/D3D12")

    add_custom_command(TARGET ${exe_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${dst_expr}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${src_dir}/D3D12Core.dll"
            "${dst_expr}/D3D12Core.dll"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${src_dir}/d3d12SDKLayers.dll"
            "${dst_expr}/d3d12SDKLayers.dll"
        COMMENT "[AgilitySDK] Deploying D3D12Core.dll + d3d12SDKLayers.dll -> D3D12/"
        VERBATIM
    )
endfunction()
