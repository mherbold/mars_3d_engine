# =============================================================================
# NsightAftermath.cmake
# Wires the NVIDIA Nsight Aftermath SDK into the CMake build.
#
# Aftermath provides GPU crash dump capture and decoding. When an application
# detects a device-removed (DXGI_ERROR_DEVICE_REMOVED) event it can call
# Aftermath to write a structured .nv-gpudmp file which Nsight Graphics can
# decode to show the exact shader instruction and pipeline state at the point
# of the GPU fault.
#
# Integration model:
#   - At build time: link GFSDK_Aftermath_Lib.x64.lib.
#   - At runtime:    GFSDK_Aftermath_Lib.x64.dll must be beside the exe.
#   - In source:     #include <GFSDK_Aftermath.h> (and the crash-dump headers).
#
# Aftermath is intentionally kept as a PRIVATE engine dependency — the public
# engine API does not expose Aftermath types. The engine initialises Aftermath
# during device creation (when MARS_ENABLE_AFTERMATH is ON) and the crash-dump
# callback writes .nv-gpudmp files to a configurable output directory.
#
# Cache variables:
#   AFTERMATH_SDK_DIR       — root of the Aftermath SDK (contains include/, lib/)
#   MARS_ENABLE_AFTERMATH   — ON  = build with crash-dump support (Debug default)
#                             OFF = strip Aftermath entirely
# =============================================================================

cmake_minimum_required(VERSION 3.28)

set(AFTERMATH_SDK_DIR
    "C:/mars_deps/NVIDIA_Nsight_Aftermath_SDK_2025.5.0.25317-windows_x64"
    CACHE PATH "Root of the NVIDIA Nsight Aftermath SDK directory.")

option(MARS_ENABLE_AFTERMATH
    "Enable NVIDIA Nsight Aftermath GPU crash-dump capture."
    ON)

# ---------------------------------------------------------------------------
# Validate layout
# ---------------------------------------------------------------------------
if(MARS_ENABLE_AFTERMATH)
    if(EXISTS "${AFTERMATH_SDK_DIR}/include/GFSDK_Aftermath.h" AND
       EXISTS "${AFTERMATH_SDK_DIR}/lib/x64/GFSDK_Aftermath_Lib.x64.lib")
        set(AFTERMATH_SDK_FOUND TRUE)
        message(STATUS "[NsightAftermath] Found at ${AFTERMATH_SDK_DIR}")
    else()
        set(AFTERMATH_SDK_FOUND FALSE)
        message(WARNING
            "[NsightAftermath] SDK not found at AFTERMATH_SDK_DIR='${AFTERMATH_SDK_DIR}'. "
            "Set AFTERMATH_SDK_DIR to the extracted Aftermath SDK root, or set "
            "MARS_ENABLE_AFTERMATH=OFF to disable crash-dump support."
        )
    endif()
else()
    set(AFTERMATH_SDK_FOUND FALSE)
    message(STATUS "[NsightAftermath] Disabled via MARS_ENABLE_AFTERMATH=OFF.")
endif()

# ---------------------------------------------------------------------------
# IMPORTED target: MARS::NsightAftermath
# ---------------------------------------------------------------------------
if(AFTERMATH_SDK_FOUND)
    add_library(MARS::NsightAftermath STATIC IMPORTED GLOBAL)
    set_target_properties(MARS::NsightAftermath PROPERTIES
        # Single release-mode static import lib (no debug variant provided)
        IMPORTED_LOCATION             "${AFTERMATH_SDK_DIR}/lib/x64/GFSDK_Aftermath_Lib.x64.lib"
        IMPORTED_LOCATION_DEBUG       "${AFTERMATH_SDK_DIR}/lib/x64/GFSDK_Aftermath_Lib.x64.lib"
        IMPORTED_LOCATION_RELEASE     "${AFTERMATH_SDK_DIR}/lib/x64/GFSDK_Aftermath_Lib.x64.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${AFTERMATH_SDK_DIR}/include"
        # Propagate the compile definition so #ifdef MARS_AFTERMATH_ENABLED
        # guards work in engine source without callers setting it manually
        INTERFACE_COMPILE_DEFINITIONS "MARS_AFTERMATH_ENABLED=1"
    )
endif()

# ---------------------------------------------------------------------------
# mars_deploy_aftermath(<exe_target>)
#
# Post-build step: copies GFSDK_Aftermath_Lib.x64.dll beside the executable.
# ---------------------------------------------------------------------------
function(mars_deploy_aftermath exe_target)
    if(NOT AFTERMATH_SDK_FOUND)
        return()
    endif()

    set(dll_src "${AFTERMATH_SDK_DIR}/lib/x64/GFSDK_Aftermath_Lib.x64.dll")
    set(dll_dst "$<TARGET_FILE_DIR:${exe_target}>/GFSDK_Aftermath_Lib.x64.dll")

    add_custom_command(TARGET ${exe_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${dll_src}"
            "${dll_dst}"
        COMMENT "[NsightAftermath] Deploying GFSDK_Aftermath_Lib.x64.dll"
        VERBATIM
    )
endfunction()
