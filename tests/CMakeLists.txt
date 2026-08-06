include(FetchContent)

# Catch2 via FetchContent rather than vcpkg: the test job then needs no package
# manager, no Skyrim SDK, and no Windows-specific toolchain.
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(csgov_tests
    test_presets.cpp
    test_stats.cpp
    test_cycler.cpp
    test_governor.cpp
)

target_link_libraries(csgov_tests PRIVATE csgov_core Catch2::Catch2WithMain)
target_compile_features(csgov_tests PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(csgov_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(csgov_tests PRIVATE -Wall -Wextra)
endif()

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(csgov_tests)
