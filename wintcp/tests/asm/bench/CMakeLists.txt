# CMakeLists.txt for wintcp Assembly Optimization Benchmark
# SPDX-License-Identifier: Apache-2.0
#
# Builds the two benchmark executables and the optimized static library:
#
#   bench_orig.exe  - wintcp_benchmark.cpp linked with orig_functions.cpp
#                     (the original C++ implementations, faithful copies)
#
#   bench_asm.exe   - wintcp_benchmark.cpp linked with replaced_functions.cpp
#                     (originals commented out) + asm_functions.cpp (delegates
#                     to the optimized implementations) + wintcp_asm_opt.lib
#
#   wintcp_asm_opt.lib  - the optimized implementations from opt_functions.cpp
#
# The SAME wintcp_benchmark.cpp harness compiles into both executables; the only
# difference is which .cpp files provide the seven bench_api functions. Because
# both variants implement the identical bench_api.h interface, a single harness
# binary is compiled and linked against two different code providers.
#
# Usage (Developer Command Prompt for VS 2022):
#   cmake -S wintcp/tests/asm/bench -B wintcp/tests/asm/bench/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build wintcp/tests/asm/bench/build
#   # Or with the VS generator:
#   cmake -S wintcp/tests/asm/bench -B wintcp/tests/asm/bench/build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
#   cmake --build wintcp/tests/asm/bench/build --config Release
#
# The executables are placed in the build output directory.
# Run benchmarks:
#   .\wintcp_asm_opt_build\bench_orig.exe --json
#   .\wintcp_asm_opt_build\bench_asm.exe --json
#
# Or use build_bench.bat at the repository root for a direct cl.exe path.

cmake_minimum_required(VERSION 3.20)
project(wintcp_benchmark VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -- Paths -----------------------------------------------------------------
# The benchmark harness lives in the asm directory (parent of bench/).
get_filename_component(BENCH_HARNESS
    "${CMAKE_CURRENT_SOURCE_DIR}/../wintcp_benchmark.cpp"
    ABSOLUTE)

# The asm directory (parent of bench/) holds opt_functions.h / .cpp.
set(ASM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Include paths:
#   ${CMAKE_CURRENT_SOURCE_DIR}  -> bench_api.h
#   ${ASM_DIR}                   -> opt_functions.h
set(BENCH_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${ASM_DIR}"
)

# -- Compile definitions matching the wintcp project conventions --
set(BENCH_COMPILE_DEFS
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0601
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ============================================================================
# wintcp_asm_opt.lib  -- the optimized implementations as a static library
# ============================================================================
add_library(wintcp_asm_opt STATIC
    "${ASM_DIR}/opt_functions.cpp"
)
target_include_directories(wintcp_asm_opt PUBLIC ${BENCH_INCLUDE_DIRS})
target_compile_definitions(wintcp_asm_opt PRIVATE ${BENCH_COMPILE_DEFS})

# ============================================================================
# bench_orig.exe  -- original C++ implementations
# ============================================================================
add_executable(bench_orig
    "${BENCH_HARNESS}"
    "${CMAKE_CURRENT_SOURCE_DIR}/orig_functions.cpp"
)
target_include_directories(bench_orig PRIVATE ${BENCH_INCLUDE_DIRS})
target_compile_definitions(bench_orig PRIVATE ${BENCH_COMPILE_DEFS})

# ============================================================================
# bench_asm.exe  -- optimized (assembly/intrinsic) implementations
# ============================================================================
add_executable(bench_asm
    "${BENCH_HARNESS}"
    "${CMAKE_CURRENT_SOURCE_DIR}/replaced_functions.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/asm_functions.cpp"
)
target_link_libraries(bench_asm PRIVATE wintcp_asm_opt)
target_include_directories(bench_asm PRIVATE ${BENCH_INCLUDE_DIRS})
target_compile_definitions(bench_asm PRIVATE ${BENCH_COMPILE_DEFS})

# ============================================================================
# MSVC-specific options
# ============================================================================
if(MSVC)
    foreach(target bench_orig bench_asm wintcp_asm_opt)
        target_compile_options(${target} PRIVATE
            /W4
            /EHsc
            /utf-8
            /permissive-
            /Zc:__cplusplus
            /guard:cf
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Debug>:/Od /Zi>
        )
        # Static CRT (no DLL dependencies) -- matches the wintcp build.
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()

    set_target_properties(bench_orig bench_asm PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    set_target_properties(wintcp_asm_opt PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )

    # Ws2_32 for InetNtopW (orig Ipv4/Ipv6 copies); Advapi32 for the
    # registry/clock calls in the harness.
    target_link_libraries(bench_orig PRIVATE ws2_32 advapi32)
    target_link_libraries(bench_asm PRIVATE ws2_32 advapi32)
endif()
