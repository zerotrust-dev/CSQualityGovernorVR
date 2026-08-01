# SKSE plugin target. Requires CommonLibSSE-NG, supplied through vcpkg.
#
# This half cannot be validated locally - there is no dev kit on the machine -
# so expect CI iteration here. The core library and its tests are independent
# of everything below.

find_package(CommonLibSSE CONFIG REQUIRED)

add_commonlibsse_plugin(CSQualityGovernorVR
    SOURCES
        main.cpp
        Config.cpp
        Reporter.cpp
    AUTHOR "zerotrust-dev"
    NAME "CSQualityGovernorVR"
)

target_include_directories(CSQualityGovernorVR PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(CSQualityGovernorVR PRIVATE csgov_core)
target_compile_features(CSQualityGovernorVR PRIVATE cxx_std_20)

target_precompile_headers(CSQualityGovernorVR PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/PCH.h
)
