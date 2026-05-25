cmake_minimum_required(VERSION 3.20)
project(lulzfish
    VERSION 0.1.0
    LANGUAGES CXX
)

# ==============================================================================
# Language & Compiler Setup
# ==============================================================================

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release with debug info if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

# ==============================================================================
# Options
# ==============================================================================

option(LULZFISH_ENABLE_NATIVE "Enable -march=native (good for dev machines)" ON)
option(LULZFISH_ENABLE_LTO "Enable Link Time Optimization in Release" ON)
option(LULZFISH_BUILD_TESTS "Build unit/perft tests" ON)

# ==============================================================================
# Compiler Flags
# ==============================================================================

if(MSVC)
    # MSVC flags (less optimized path for now)
    add_compile_options(/W4 /EHsc /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    # Clang / GCC
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
        -Wformat=2
    )

    if(LULZFISH_ENABLE_NATIVE)
        add_compile_options(-march=native)
    endif()

    # Optimization levels per build type
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g" CACHE STRING "" FORCE)
endif()

if(LULZFISH_ENABLE_LTO AND NOT MSVC)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    else()
        message(WARNING "IPO/LTO not supported: ${ipo_error}")
    endif()
endif()

# ==============================================================================
# Sources
# ==============================================================================

set(LULZFISH_CORE_SOURCES
    src/lulzfish/core/types.hpp
    src/lulzfish/core/bitboard.hpp
    src/lulzfish/core/position.hpp
    src/lulzfish/core/movegen.hpp
)

# For now we use header-only or single-file implementations where practical.
# We will split into .cpp as the project grows.

set(LULZFISH_CORE_SOURCES
    src/lulzfish/core/position.cpp
    src/lulzfish/core/attacks.cpp
    src/lulzfish/core/movegen.cpp
    src/lulzfish/eval/material.cpp
    src/lulzfish/eval/graph_eval.cpp
    src/lulzfish/search/search.cpp
    src/lulzfish/search/transposition.cpp
    src/lulzfish/uci/uci.cpp
)

set(LULZFISH_SOURCES
    src/main.cpp
    ${LULZFISH_CORE_SOURCES}
)

# ==============================================================================
# Main Executable
# ==============================================================================

add_executable(lulzfish ${LULZFISH_SOURCES})

target_include_directories(lulzfish
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(lulzfish PRIVATE cxx_std_23)

# ==============================================================================
# Tests (optional)
# ==============================================================================

if(LULZFISH_BUILD_TESTS)
    # Simple perft test executable for now (no heavy test framework yet)
    add_executable(perft_test
        tests/perft_test.cpp
        ${LULZFISH_CORE_SOURCES}
    )
    target_include_directories(perft_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(perft_test PRIVATE cxx_std_23)

    add_executable(search_regression
        tests/search_regression.cpp
        ${LULZFISH_CORE_SOURCES}
    )
    target_include_directories(search_regression
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(search_regression PRIVATE cxx_std_23)

    # Later we can add Catch2 or a custom harness
endif()

# ==============================================================================
# Installation / Packaging (future)
# ==============================================================================

install(TARGETS lulzfish
    RUNTIME DESTINATION bin
)

# ==============================================================================
# Summary
# ==============================================================================

message(STATUS "Lulzfish configuration:")
message(STATUS "  Build type:           ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:         ${CMAKE_CXX_STANDARD}")
message(STATUS "  Native optimizations: ${LULZFISH_ENABLE_NATIVE}")
message(STATUS "  LTO:                  ${LULZFISH_ENABLE_LTO}")
message(STATUS "  Build tests:          ${LULZFISH_BUILD_TESTS}")
