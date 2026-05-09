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

# Sub-projects (order matters: engine must be defined before test_app)
add_subdirectory(engine)
add_subdirectory(test_app)
