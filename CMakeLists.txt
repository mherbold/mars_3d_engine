cmake_minimum_required(VERSION 3.28)

project(MARS_3D_Engine
    VERSION 0.1.0
    DESCRIPTION "MARS (Marvin's Awesome Racing Simulator) Full Path Tracing 3D Engine"
    LANGUAGES CXX
)

# Require C++23 for modern language features
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Windows is the only supported platform
if(NOT WIN32)
    message(FATAL_ERROR "MARS 3D Engine is a Windows-only project.")
endif()

# Place all build outputs in a single, well-known location
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# =============================================================================
# Build options
# =============================================================================
option(MARS_ENABLE_DEV_UI
    "Enable the developer / debug overlay (ImGui, PIX markers, Aftermath). \
Compile-in only for Debug / development builds."
    OFF)

# =============================================================================
# CMake module path (cmake/ directory contains our custom helper modules)
# =============================================================================
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# =============================================================================
# Third-party: Windows Agility SDK (D3D12 1.614+)
# Must be included BEFORE FetchContent so engine and test_app can reference it.
# =============================================================================
include(AgilitySDK)
include(DirectXLibs)
include(StreamlineSDK)
include(NsightAftermath)
include(FetchContent)

# --- Assimp (3D model importer: FBX, glTF, OBJ) ---
FetchContent_Declare(assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v5.4.3
    GIT_SHALLOW    TRUE
)
set(ASSIMP_BUILD_TESTS         OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL             OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER  ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER  ON  CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT           ON  CACHE BOOL "" FORCE)

# --- nlohmann/json (JSON parsing for .marsscene, .marsui, themes) ---
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)

# --- D3D12 Memory Allocator (D3D12MA by AMD/GPUOpen) ---
FetchContent_Declare(D3D12MemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.git
    GIT_TAG        v2.0.1
    GIT_SHALLOW    TRUE
)
set(D3D12MA_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)

# --- HarfBuzz (Unicode text shaping for the game UI) ---
FetchContent_Declare(harfbuzz
    GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
    GIT_TAG        9.0.0
    GIT_SHALLOW    TRUE
)
set(HB_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(HB_BUILD_UTILS   OFF CACHE BOOL "" FORCE)
set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)

# --- FreeType (font metrics; used by msdfgen and HarfBuzz at build time) ---
FetchContent_Declare(freetype
    GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
    GIT_TAG        VER-2-13-3
    GIT_SHALLOW    TRUE
)
set(FT_DISABLE_ZLIB   ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2  ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)

# --- msdfgen (offline MSDF atlas generation; build-tool only, not linked at runtime) ---
FetchContent_Declare(msdfgen
    GIT_REPOSITORY https://github.com/Chlumsky/msdfgen.git
    GIT_TAG        v1.12
    GIT_SHALLOW    TRUE
)
set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_FREETYPE     OFF CACHE BOOL "" FORCE)
set(MSDFGEN_CORE_ONLY        ON  CACHE BOOL "" FORCE)
set(MSDFGEN_USE_VCPKG        OFF CACHE BOOL "" FORCE)

# --- ImGui (developer overlay; included only when MARS_ENABLE_DEV_UI is ON) ---
if(MARS_ENABLE_DEV_UI)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.8
        GIT_SHALLOW    TRUE
    )
endif()

# Make all declared packages available (downloads + configures)
# FETCHCONTENT_UPDATES_DISCONNECTED prevents re-download/re-clone when the
# source directory already exists (avoids OneDrive file-lock issues).
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
if(MARS_ENABLE_DEV_UI)
    FetchContent_MakeAvailable(assimp nlohmann_json D3D12MemoryAllocator harfbuzz freetype msdfgen imgui)
else()
    FetchContent_MakeAvailable(assimp nlohmann_json D3D12MemoryAllocator harfbuzz freetype msdfgen)
endif()

# =============================================================================
# Sub-projects (order matters: engine must be defined before test_app)
# =============================================================================
add_subdirectory(engine)
add_subdirectory(test_app)

# =============================================================================
# Offline asset-conditioning tools (standalone executables linked against
# mars_engine — run on demand to generate pre-baked atlas assets).
# =============================================================================
add_subdirectory(tools/impostor_baker)
add_subdirectory(tools/omm_baker)
