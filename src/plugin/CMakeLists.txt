# SKSE plugin target.
#
# Pattern copied from the sibling HeadDirectedTurning / TiptoeToJumpVR repos,
# which already build green in CI. Do not "improve" it without checking those
# first - the package name (commonlibsse-ng-vr, not commonlibsse-ng) and the
# COMPATIBILITY VR flag are both load-bearing.

find_package(CommonLibSSE CONFIG REQUIRED)

add_commonlibsse_plugin(
    CSQualityGovernorVR
    AUTHOR "zerotrust-dev"
    NAME "CS Quality Governor VR"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY VR
)

target_sources(
    CSQualityGovernorVR
    PRIVATE
        main.cpp
        Config.cpp
        Config.h
        ApiProbe.cpp
        ApiProbe.h
        Reporter.cpp
        Reporter.h
        CompositorTimer.cpp
        CompositorTimer.h
        FrameGpuTimer.cpp
        FrameGpuTimer.h
        PCH.h
)

# D-21: the GPU timer measures from here now, so the plugin needs D3D11 itself.
# d3d11 is part of the Windows SDK, so this adds no package dependency - which
# is the point: nothing here has to be kept in step with whatever Community
# Shaders build the modlist ships.
target_link_libraries(CSQualityGovernorVR PRIVATE d3d11)

target_include_directories(CSQualityGovernorVR PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(CSQualityGovernorVR PRIVATE csgov_core)
target_precompile_headers(CSQualityGovernorVR PRIVATE PCH.h)

if(MSVC)
    target_compile_options(CSQualityGovernorVR PRIVATE /W4 /permissive- /EHsc)
endif()
