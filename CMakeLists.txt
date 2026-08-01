cmake_minimum_required(VERSION 3.24)
project(CSQualityGovernorVR VERSION 0.1.0 LANGUAGES CXX)

# C++23 to match CommonLibSSE-NG, as the sibling plugin repos do. The core
# library only needs C++20 and declares that as its minimum, so the test job
# stays buildable on older toolchains.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CSGOV_BUILD_TESTS "Build the core unit tests" ON)
option(CSGOV_BUILD_PLUGIN "Build the SKSE plugin (requires CommonLibSSE-NG)" OFF)

# ---------------------------------------------------------------------------
# core: pure logic, no SKSE / no Skyrim SDK / no Windows dependency.
# This is deliberate - it is what makes the tests runnable in CI without the
# game or its toolchain.
# ---------------------------------------------------------------------------
add_library(csgov_core STATIC
    src/core/Presets.cpp
    src/core/Stats.cpp
    src/core/CyclerCore.cpp
)

target_include_directories(csgov_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(csgov_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(csgov_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(csgov_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CSGOV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CSGOV_BUILD_PLUGIN)
    add_subdirectory(src/plugin)
endif()
