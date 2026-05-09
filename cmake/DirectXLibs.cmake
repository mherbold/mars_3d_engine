# =============================================================================
# DirectXLibs.cmake
# Creates CMake IMPORTED targets for the three DirectX toolkit NuGet packages:
#
#   MARS::DirectXTK12   — DirectX Tool Kit for DirectX 12
#   MARS::DirectXTex    — DirectX Texture Processing Library
#   MARS::DirectXMesh   — DirectX Mesh Geometry Processing Library
#
# Each target selects Debug or Release .lib automatically via generator
# expressions, matching the active build configuration.
#
# Cache variables (override if you installed to a different path):
#   DIRECTXTK12_DIR   — root of directxtk12_desktop_win10.<ver>/
#   DIRECTXTEX_DIR    — root of directxtex_desktop_win10.<ver>/
#   DIRECTXMESH_DIR   — root of directxmesh_desktop_win10.<ver>/
# =============================================================================

cmake_minimum_required(VERSION 3.28)

# ---------------------------------------------------------------------------
# Default paths — the layout produced by the nuget install command
# ---------------------------------------------------------------------------
set(DIRECTXTK12_DIR
    "C:/mars_deps/nuget/directxtk12_desktop_win10.2026.4.1.1"
    CACHE PATH "Root of the DirectXTK12 NuGet package directory.")

set(DIRECTXTEX_DIR
    "C:/mars_deps/nuget/directxtex_desktop_win10.2026.5.8.1"
    CACHE PATH "Root of the DirectXTex NuGet package directory.")

set(DIRECTXMESH_DIR
    "C:/mars_deps/nuget/directxmesh_desktop_win10.2026.5.8.1"
    CACHE PATH "Root of the DirectXMesh NuGet package directory.")

# ---------------------------------------------------------------------------
# Helper: create one IMPORTED STATIC target
#
#   _mars_add_dx_lib(<target_name> <root_dir> <lib_basename>)
#
#   Expects the NuGet layout:
#     <root_dir>/include/             — public headers
#     <root_dir>/native/lib/x64/Debug/<lib_basename>.lib
#     <root_dir>/native/lib/x64/Release/<lib_basename>.lib
# ---------------------------------------------------------------------------
function(_mars_add_dx_lib target_name root_dir lib_basename)
    set(inc_dir  "${root_dir}/include")
    set(lib_dbg  "${root_dir}/native/lib/x64/Debug/${lib_basename}.lib")
    set(lib_rel  "${root_dir}/native/lib/x64/Release/${lib_basename}.lib")

    if(NOT EXISTS "${inc_dir}" OR NOT EXISTS "${lib_dbg}")
        message(WARNING
            "[DirectXLibs] ${target_name}: package not found at '${root_dir}'. "
            "Set ${target_name}_DIR to the correct NuGet package root."
        )
        return()
    endif()

    add_library(${target_name} STATIC IMPORTED GLOBAL)

    set_target_properties(${target_name} PROPERTIES
        IMPORTED_LOCATION                "${lib_rel}"
        IMPORTED_LOCATION_DEBUG          "${lib_dbg}"
        IMPORTED_LOCATION_RELEASE        "${lib_rel}"
        IMPORTED_LOCATION_RELWITHDEBINFO "${lib_rel}"
        IMPORTED_LOCATION_MINSIZEREL     "${lib_rel}"
        INTERFACE_INCLUDE_DIRECTORIES    "${inc_dir}"
    )

    message(STATUS "[DirectXLibs] ${target_name} -> ${root_dir}")
endfunction()

# ---------------------------------------------------------------------------
# Create the three IMPORTED targets
# ---------------------------------------------------------------------------
_mars_add_dx_lib(MARS::DirectXTK12  "${DIRECTXTK12_DIR}"  "DirectXTK12")
_mars_add_dx_lib(MARS::DirectXTex   "${DIRECTXTEX_DIR}"   "DirectXTex")
_mars_add_dx_lib(MARS::DirectXMesh  "${DIRECTXMESH_DIR}"  "DirectXMesh")
