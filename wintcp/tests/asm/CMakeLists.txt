# CMakeLists.txt for Assembly Optimization Test Project
# SPDX-License-Identifier: Apache-2.0
#
# Builds a standalone test executable that validates the assembly-optimized
# functions that replace hot paths in wintcp.exe.
#
# Usage (Developer Command Prompt for VS 2022):
#   cmake -S wintcp/tests/asm -B wintcp/tests/asm/build -G "Visual Studio 17 2022" -A x64
#   cmake --build wintcp/tests/asm/build --config Release
#
# Or with Ninja:
#   cmake -S wintcp/tests/asm -B wintcp/tests/asm/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build wintcp/tests/asm/build

cmake_minimum_required(VERSION 3.20)
project(testAssemblies VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The test executable
add_executable(testAssemblies
    testAssemblies.cpp
    opt_functions.cpp
)

# Include the wintcp source directory for headers
target_include_directories(testAssemblies PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

# Compile definitions matching the wintcp project
target_compile_definitions(testAssemblies PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0601
)

# Output to a separate directory to avoid confusion with wintcp.exe
set_target_properties(testAssemblies PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

if(MSVC)
    target_compile_options(testAssemblies PRIVATE
        /W4 /EHsc /utf-8 /permissive- /Zc:__cplusplus
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Debug>:/Od /Zi>
    )

    # Static CRT for self-contained binary
    set_property(TARGET testAssemblies PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    target_link_options(testAssemblies PRIVATE /MANIFEST:NO)
endif()

# Link required libraries
target_link_libraries(testAssemblies PRIVATE
    iphlpapi
    ws2_32
    comctl32
    user32
)
