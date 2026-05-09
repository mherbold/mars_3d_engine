# =============================================================================
# StreamlineSDK.cmake
# Wires the NVIDIA Streamline SDK v2.x into the CMake build.
#
# Streamline is the unified NVIDIA framework that provides:
#   - DLSS 4 Super Resolution          (sl.dlss.dll   / nvngx_dlss.dll)
#   - DLSS 4 Frame Generation          (sl.dlss_g.dll / nvngx_dlssg.dll)
#   - DLSS 4 Ray Reconstruction        (sl.dlss_d.dll / nvngx_dlssd.dll)
#   - NVIDIA Reflex (low-latency)      (sl.reflex.dll)
#   - NIS (image sharpening)           (sl.nis.dll)
#
# Integration model:
#   - At build time: link sl.interposer.lib — this wraps IDXGIFactory /
#     ID3D12Device creation so Streamline can intercept D3D12 calls.
#   - At runtime:   all sl.*.dll and nvngx_*.dll files must be in the same
#     directory as the executable. mars_deploy_streamline() adds a post-build
#     copy step to place them there automatically.
#   - In source:    #include <sl.h> and the feature-specific headers
#     (sl_dlss.h, sl_dlss_g.h, sl_dlss_d.h) from the include/ directory.
#
# Cache variables:
#   STREAMLINE_SDK_DIR   — root of the Streamline SDK (contains include/, lib/, bin/)
# =============================================================================

cmake_minimum_required(VERSION 3.28)

set(STREAMLINE_SDK_DIR
    "C:/mars_deps/streamline-sdk-v2.11.1"
    CACHE PATH "Root of the NVIDIA Streamline SDK directory.")

# ---------------------------------------------------------------------------
# Validate layout
# ---------------------------------------------------------------------------
if(EXISTS "${STREAMLINE_SDK_DIR}/include/sl.h" AND
   EXISTS "${STREAMLINE_SDK_DIR}/lib/x64/sl.interposer.lib")
    set(STREAMLINE_SDK_FOUND TRUE)
    message(STATUS "[StreamlineSDK] Found at ${STREAMLINE_SDK_DIR}")
else()
    set(STREAMLINE_SDK_FOUND FALSE)
    message(WARNING
        "[StreamlineSDK] SDK not found at STREAMLINE_SDK_DIR='${STREAMLINE_SDK_DIR}'. "
        "Set STREAMLINE_SDK_DIR to the extracted Streamline SDK root."
    )
endif()

# ---------------------------------------------------------------------------
# IMPORTED target: MARS::Streamline
#
# Consumers link this target to get:
#   - The sl.interposer.lib import library
#   - All public Streamline headers on their include path
# ---------------------------------------------------------------------------
if(STREAMLINE_SDK_FOUND)
    add_library(MARS::Streamline STATIC IMPORTED GLOBAL)
    set_target_properties(MARS::Streamline PROPERTIES
        # sl.interposer.lib is configuration-independent (single build)
        IMPORTED_LOCATION             "${STREAMLINE_SDK_DIR}/lib/x64/sl.interposer.lib"
        IMPORTED_LOCATION_DEBUG       "${STREAMLINE_SDK_DIR}/lib/x64/sl.interposer.lib"
        IMPORTED_LOCATION_RELEASE     "${STREAMLINE_SDK_DIR}/lib/x64/sl.interposer.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${STREAMLINE_SDK_DIR}/include"
    )
endif()

# ---------------------------------------------------------------------------
# mars_deploy_streamline(<exe_target>)
#
# Post-build step: copies all required Streamline runtime DLLs into the
# same directory as the built executable.
#
# Required DLLs (must be beside the exe, not in a subdirectory):
#   Interposer + feature plugins:
#     sl.interposer.dll, sl.common.dll,
#     sl.dlss.dll, sl.dlss_g.dll, sl.dlss_d.dll,
#     sl.reflex.dll, sl.nis.dll, sl.pcl.dll,
#     sl.nvperf.dll, sl.deepdvc.dll, sl.directsr.dll
#   NGX model/runtime DLLs (the actual AI inference engines):
#     nvngx_dlss.dll    — DLSS Super Resolution AI model
#     nvngx_dlssg.dll   — DLSS Frame Generation AI model
#     nvngx_dlssd.dll   — DLSS Ray Reconstruction AI model
#     nvngx_deepdvc.dll — Deep DVC
# ---------------------------------------------------------------------------
function(mars_deploy_streamline exe_target)
    if(NOT STREAMLINE_SDK_FOUND)
        message(STATUS "[StreamlineSDK] Skipping deploy for '${exe_target}' (SDK not found).")
        return()
    endif()

    set(bin_dir "${STREAMLINE_SDK_DIR}/bin/x64")
    set(dst     "$<TARGET_FILE_DIR:${exe_target}>")

    # List of all DLLs to deploy
    set(sl_dlls
        sl.interposer.dll
        sl.common.dll
        sl.dlss.dll
        sl.dlss_g.dll
        sl.dlss_d.dll
        sl.reflex.dll
        sl.nis.dll
        sl.pcl.dll
        sl.nvperf.dll
        sl.deepdvc.dll
        sl.directsr.dll
        nvngx_dlss.dll
        nvngx_dlssg.dll
        nvngx_dlssd.dll
        nvngx_deepdvc.dll
    )

    foreach(dll IN LISTS sl_dlls)
        set(src "${bin_dir}/${dll}")
        if(EXISTS "${src}")
            add_custom_command(TARGET ${exe_target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${src}"
                    "${dst}/${dll}"
                COMMENT "[StreamlineSDK] Deploying ${dll}"
                VERBATIM
            )
        else()
            message(STATUS "[StreamlineSDK] Optional DLL not present, skipping: ${dll}")
        endif()
    endforeach()
endfunction()
