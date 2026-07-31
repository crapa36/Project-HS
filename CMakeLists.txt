cmake_minimum_required(VERSION 3.30)

project(ProjectHS VERSION 0.2.0 LANGUAGES C CXX)

include(FetchContent)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(HS_BUILD_TESTS "Build Project HS tests" ON)
option(HS_ENABLE_VALIDATION "Enable D3D12 validation" OFF)
option(HS_ENABLE_GPU_VALIDATION "Enable D3D12 GPU-based validation" OFF)

set(TF_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(TF_BUILD_CUDA OFF CACHE BOOL "" FORCE)
set(TF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TF_BUILD_MODULES OFF CACHE BOOL "" FORCE)
set(TF_BUILD_PROFILER OFF CACHE BOOL "" FORCE)
set(TF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    taskflow
    URL https://github.com/taskflow/taskflow/archive/45366fe5bc4f2f8ec9aa590b40c504e296886865.tar.gz
    URL_HASH SHA256=F10440E2630C5084B7905AF91AD0677507C6C09DA747620C7C503BE4FACFBA98
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(taskflow)

set(FLECS_SHARED OFF CACHE BOOL "" FORCE)
set(FLECS_STATIC ON CACHE BOOL "" FORCE)
set(FLECS_PIC OFF CACHE BOOL "" FORCE)
set(FLECS_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    flecs
    GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
    GIT_TAG fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(flecs)

FetchContent_Declare(
    agility
    URL https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/1.619.4
    URL_HASH SHA256=D30F756CE05BB4B7705FC1B04A5DED32ED62F2C2A2B392AE8D3318181395C8BC
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(agility)

FetchContent_Declare(
    winpix
    URL https://www.nuget.org/api/v2/package/WinPixEventRuntime/1.0.240308001
    URL_HASH SHA256=726ACC93D6968E2146261A1E415521747D50AD69894C2B42B5D0D4C29FD66EC4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(winpix)
add_library(WinPixEventRuntime SHARED IMPORTED GLOBAL)
set_target_properties(
    WinPixEventRuntime PROPERTIES
    IMPORTED_IMPLIB "${winpix_SOURCE_DIR}/bin/x64/WinPixEventRuntime.lib"
    IMPORTED_LOCATION "${winpix_SOURCE_DIR}/bin/x64/WinPixEventRuntime.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${winpix_SOURCE_DIR}/Include"
)

set(HS_FONT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Generated/Fonts")
file(MAKE_DIRECTORY "${HS_FONT_DIRECTORY}")
file(
    DOWNLOAD
    "https://raw.githubusercontent.com/google/fonts/7ff85c87f93ea6cca5f41c69f2e4edcb90240f26/ofl/notosanskr/NotoSansKR%5Bwght%5D.ttf"
    "${HS_FONT_DIRECTORY}/NotoSansKR.ttf"
    EXPECTED_HASH SHA256=194018E6B2B293A7964F037B25C0249CE1418BC9AB3C971060A03AA57861E252
    TLS_VERIFY ON
)
file(
    DOWNLOAD
    "https://raw.githubusercontent.com/google/fonts/7ff85c87f93ea6cca5f41c69f2e4edcb90240f26/ofl/notosanskr/OFL.txt"
    "${HS_FONT_DIRECTORY}/NotoSansKR-OFL.txt"
    EXPECTED_HASH SHA256=1C05C68C34F9708415AADA51F17E1B0092D2CEA709BF4A94CD38114F9E73D7D9
    TLS_VERIFY ON
)

set(D3D12MA_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(D3D12MA_AGILITY_SDK_DIRECTORY "${agility_SOURCE_DIR}" CACHE STRING "" FORCE)
FetchContent_Declare(
    d3d12ma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.git
    GIT_TAG 1d86c1130f61453634b1df85782e1fecfd59a525
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(d3d12ma)

function(hs_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_23)
    if(MSVC)
        target_compile_options(
            ${target}
            PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8
        )
    endif()
endfunction()

add_library(
    hs_core STATIC
    Source/Core/Private/fixed_step_clock.cpp
    Source/Core/Private/render_snapshot.cpp
    Source/Core/Private/snapshot_exchange.cpp
)
target_include_directories(hs_core PUBLIC Source/Core/Public)
hs_configure_target(hs_core)

add_library(
    hs_jobs STATIC
    Source/Jobs/Private/task_system.cpp
)
target_include_directories(hs_jobs PUBLIC Source/Jobs/Public)
target_link_libraries(hs_jobs PUBLIC hs_core PRIVATE Taskflow::Taskflow)
target_compile_definitions(hs_jobs PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_jobs)

add_library(
    hs_gameplay STATIC
    Source/Gameplay/Private/game_simulation.cpp
)
target_include_directories(hs_gameplay PUBLIC Source/Gameplay/Public)
target_link_libraries(hs_gameplay PUBLIC hs_core PRIVATE flecs::flecs_static)
if(MSVC)
    target_compile_options(hs_gameplay PRIVATE /fp:strict)
endif()
hs_configure_target(hs_gameplay)

add_library(
    hs_renderer_d3d12 STATIC
    Source/RendererD3D12/Private/render_graph.cpp
    Source/RendererD3D12/Private/renderer.cpp
)
target_include_directories(hs_renderer_d3d12 PUBLIC Source/RendererD3D12/Public)
target_include_directories(
    hs_renderer_d3d12
    BEFORE PRIVATE "${agility_SOURCE_DIR}/build/native/include"
)
target_link_libraries(
    hs_renderer_d3d12
    PUBLIC hs_core
    PRIVATE D3D12MemoryAllocator WinPixEventRuntime d3d12 dxgi dxguid windowscodecs gdi32
)
target_compile_definitions(hs_renderer_d3d12 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
if(HS_ENABLE_VALIDATION)
    target_compile_definitions(hs_renderer_d3d12 PRIVATE USE_PIX=1)
else()
    target_compile_definitions(hs_renderer_d3d12 PRIVATE USE_PIX_RETAIL=1)
endif()
hs_configure_target(hs_renderer_d3d12)

add_library(
    hs_runtime STATIC
    Source/Runtime/Private/application.cpp
    Source/Runtime/Private/window.cpp
)
target_include_directories(hs_runtime PUBLIC Source/Runtime/Public)
target_link_libraries(
    hs_runtime
    PUBLIC hs_core
    PRIVATE hs_gameplay hs_jobs hs_renderer_d3d12 user32 shell32
)
target_compile_definitions(hs_runtime PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
hs_configure_target(hs_runtime)

find_program(
    HS_DXC
    NAMES dxc.exe
    REQUIRED
    HINTS
        "$ENV{WindowsSdkBinPath}/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
)
set(HS_SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/stage1.hlsl")
set(HS_SHADER_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Generated/Shaders")
file(MAKE_DIRECTORY "${HS_SHADER_DIRECTORY}")

function(hs_compile_shader output entry profile)
    add_custom_command(
        OUTPUT "${HS_SHADER_DIRECTORY}/${output}"
        COMMAND
            "${HS_DXC}" -nologo -WX -O3 -Ges -E "${entry}" -T "${profile}"
            -Fo "${HS_SHADER_DIRECTORY}/${output}" "${HS_SHADER_SOURCE}"
        DEPENDS "${HS_SHADER_SOURCE}"
        VERBATIM
    )
    set(HS_SHADER_OUTPUTS ${HS_SHADER_OUTPUTS} "${HS_SHADER_DIRECTORY}/${output}" PARENT_SCOPE)
endfunction()

hs_compile_shader(scene_vs.dxil SceneVS vs_6_0)
hs_compile_shader(scene_ps.dxil ScenePS ps_6_0)
hs_compile_shader(shadow_vs.dxil ShadowVS vs_6_0)
hs_compile_shader(particle_vs.dxil ParticleVS vs_6_0)
hs_compile_shader(particle_ps.dxil ParticlePS ps_6_0)
hs_compile_shader(particle_cs.dxil ParticleCS cs_6_0)
hs_compile_shader(fullscreen_vs.dxil FullScreenVS vs_6_0)
hs_compile_shader(deferred_ps.dxil DeferredPS ps_6_0)
hs_compile_shader(composite_ps.dxil CompositePS ps_6_0)
hs_compile_shader(bloom_ps.dxil BloomPS ps_6_0)
hs_compile_shader(tonemap_ps.dxil ToneMapPS ps_6_0)
hs_compile_shader(outline_ps.dxil OutlinePS ps_6_0)
hs_compile_shader(fxaa_ps.dxil FxaaPS ps_6_0)
hs_compile_shader(ui_ps.dxil UiPS ps_6_0)
add_custom_target(hs_shaders ALL DEPENDS ${HS_SHADER_OUTPUTS})

add_custom_target(
    hs_runtime_assets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/Shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${HS_SHADER_OUTPUTS}
        "${CMAKE_CURRENT_BINARY_DIR}/Shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${agility_SOURCE_DIR}/build/native/bin/x64/D3D12Core.dll"
        "${CMAKE_CURRENT_BINARY_DIR}/D3D12/D3D12Core.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${agility_SOURCE_DIR}/build/native/bin/x64/d3d12SDKLayers.dll"
        "${CMAKE_CURRENT_BINARY_DIR}/D3D12/d3d12SDKLayers.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${winpix_SOURCE_DIR}/bin/x64/WinPixEventRuntime.dll"
        "${CMAKE_CURRENT_BINARY_DIR}/WinPixEventRuntime.dll"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/Fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${HS_FONT_DIRECTORY}/NotoSansKR.ttf"
        "${CMAKE_CURRENT_BINARY_DIR}/Fonts/NotoSansKR.ttf"
    DEPENDS hs_shaders
    VERBATIM
)

add_executable(ProjectHS Source/Apps/ProjectHS/main.cpp)
target_link_libraries(ProjectHS PRIVATE hs_runtime)
target_include_directories(ProjectHS BEFORE PRIVATE "${agility_SOURCE_DIR}/build/native/include")
target_compile_definitions(ProjectHS PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
if(HS_ENABLE_VALIDATION)
    target_compile_definitions(ProjectHS PRIVATE HS_ENABLE_VALIDATION=1)
endif()
if(HS_ENABLE_GPU_VALIDATION)
    target_compile_definitions(ProjectHS PRIVATE HS_ENABLE_GPU_VALIDATION=1)
endif()
hs_configure_target(ProjectHS)
add_dependencies(ProjectHS hs_runtime_assets)

add_executable(hs_experiment Source/Apps/Experiment/main.cpp)
target_link_libraries(hs_experiment PRIVATE hs_runtime)
target_compile_definitions(
    hs_experiment PRIVATE HS_ENABLE_VALIDATION=1 WIN32_LEAN_AND_MEAN NOMINMAX
)
hs_configure_target(hs_experiment)
add_dependencies(hs_experiment hs_runtime_assets)

set(
    HS_FBX_SDK_ROOT
    "C:/Program Files/Autodesk/FBX/FBX SDK/2020.3.7"
    CACHE PATH
    "Autodesk FBX SDK 2020.3.7 VS2022 root"
)
if(NOT EXISTS "${HS_FBX_SDK_ROOT}/include/fbxsdk.h")
    message(FATAL_ERROR "FBX SDK 2020.3.7 VS2022 not found: ${HS_FBX_SDK_ROOT}")
endif()

add_library(fbxsdk STATIC IMPORTED GLOBAL)
set_target_properties(
    fbxsdk PROPERTIES
    IMPORTED_LOCATION "${HS_FBX_SDK_ROOT}/lib/x64/release/libfbxsdk-mt.lib"
    IMPORTED_LOCATION_DEBUG "${HS_FBX_SDK_ROOT}/lib/x64/debug/libfbxsdk-mt.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${HS_FBX_SDK_ROOT}/include"
)
add_library(fbx_xml STATIC IMPORTED GLOBAL)
set_target_properties(
    fbx_xml PROPERTIES
    IMPORTED_LOCATION "${HS_FBX_SDK_ROOT}/lib/x64/release/libxml2-mt.lib"
    IMPORTED_LOCATION_DEBUG "${HS_FBX_SDK_ROOT}/lib/x64/debug/libxml2-mt.lib"
)
add_library(fbx_zlib STATIC IMPORTED GLOBAL)
set_target_properties(
    fbx_zlib PROPERTIES
    IMPORTED_LOCATION "${HS_FBX_SDK_ROOT}/lib/x64/release/zlib-mt.lib"
    IMPORTED_LOCATION_DEBUG "${HS_FBX_SDK_ROOT}/lib/x64/debug/zlib-mt.lib"
)

add_executable(hs_content Source/Apps/Content/main.cpp)
target_link_libraries(hs_content PRIVATE fbxsdk fbx_xml fbx_zlib ws2_32)
add_dependencies(hs_content hs_shaders)
target_compile_definitions(
    hs_content
    PRIVATE
        HS_COOKED_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}/Cooked"
        HS_SHADER_DIRECTORY="${HS_SHADER_DIRECTORY}"
)
target_include_directories(hs_content SYSTEM PRIVATE "${HS_FBX_SDK_ROOT}/include")
hs_configure_target(hs_content)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Cooked/manifest.txt"
    COMMAND "$<TARGET_FILE:hs_content>" --validate-only
    DEPENDS hs_content hs_shaders
    BYPRODUCTS
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/stage1_archer.meshbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/stage1_white.dds"
    VERBATIM
)
add_custom_target(
    hs_cooked_content ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/Cooked/manifest.txt"
)
add_dependencies(ProjectHS hs_cooked_content)
add_dependencies(hs_experiment hs_cooked_content)

if(HS_BUILD_TESTS)
    enable_testing()
    add_executable(hs_tests Tests/foundation_tests.cpp)
    target_link_libraries(hs_tests PRIVATE hs_core hs_jobs hs_gameplay)
    hs_configure_target(hs_tests)

    add_test(NAME foundation.stage1 COMMAND hs_tests)
    add_test(NAME adr.task_system COMMAND hs_tests)
    add_test(NAME content.validate COMMAND hs_content --validate-only)
    add_test(
        NAME smoke.stage1
        COMMAND
            ProjectHS --smoke --ticks=180 --barriers=legacy
            --resize-test
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/stage1
    )
    set_tests_properties(smoke.stage1 PROPERTIES TIMEOUT 120)
    add_test(
        NAME barriers.stage1
        COMMAND
            powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Tests/compare_barriers.ps1"
            -Executable "$<TARGET_FILE:ProjectHS>"
            -Artifacts "${CMAKE_CURRENT_BINARY_DIR}/Artifacts/barriers"
    )
    set_tests_properties(barriers.stage1 PROPERTIES TIMEOUT 120)
    add_test(
        NAME experiment.stage1
        COMMAND
            hs_experiment
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/experiment
    )
    set_tests_properties(experiment.stage1 PROPERTIES TIMEOUT 120)
endif()

install(
    TARGETS ProjectHS
    RUNTIME DESTINATION .
    COMPONENT Runtime
)
install(
    FILES "${agility_SOURCE_DIR}/build/native/bin/x64/D3D12Core.dll"
    DESTINATION D3D12
    COMPONENT Runtime
)
install(
    DIRECTORY "${HS_SHADER_DIRECTORY}/"
    DESTINATION Shaders
    COMPONENT Runtime
    FILES_MATCHING PATTERN "*.dxil"
)
install(
    FILES "${winpix_SOURCE_DIR}/bin/x64/WinPixEventRuntime.dll"
    DESTINATION .
    COMPONENT Runtime
)
install(
    FILES "${HS_FONT_DIRECTORY}/NotoSansKR.ttf"
    DESTINATION Fonts
    COMPONENT Runtime
)
install(
    FILES "${HS_FONT_DIRECTORY}/NotoSansKR-OFL.txt"
    DESTINATION Licenses
    COMPONENT Runtime
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/stage1_archer.meshbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/stage1_white.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/manifest.txt"
    DESTINATION Cooked
    COMPONENT Runtime
)
install(FILES ThirdPartyNotices.txt DESTINATION . COMPONENT Runtime)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME ProjectHS)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(
    CPACK_INSTALL_CMAKE_PROJECTS
    "${CMAKE_BINARY_DIR};${PROJECT_NAME};Runtime;/"
)
include(CPack)
