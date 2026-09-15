cmake_minimum_required(VERSION 3.30)

project(ProjectHS VERSION 0.2.0 LANGUAGES C CXX)

include(FetchContent)

if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    if(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX STREQUAL "李멸퀬: ?ы븿 ?뚯씪: ")
        # CMake can transcode the Korean MSVC prefix twice while probing cl.exe.
        # Ninja compares bytes, so restore the actual UTF-8 compiler output.
        set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "참고: 포함 파일: ")
        set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_CXX_CL_SHOWINCLUDES_PREFIX}")
    endif()
    if(NOT CMAKE_CXX_CL_SHOWINCLUDES_PREFIX STREQUAL "Note: including file: " AND
       NOT CMAKE_CXX_CL_SHOWINCLUDES_PREFIX STREQUAL "참고: 포함 파일: ")
        message(FATAL_ERROR
            "MSVC/Ninja header dependency tracking detected an unsupported /showIncludes prefix. "
            "Reconfigure with 'cmake --fresh --preset <msvc-preset>'. "
            "Detected prefix: '${CMAKE_CXX_CL_SHOWINCLUDES_PREFIX}'"
        )
    endif()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(HS_BUILD_TESTS "Build Project HS tests" ON)
option(HS_BUILD_RUNTIME "Build the D3D12 runtime and applications" ON)
option(HS_BUILD_CONTENT_TOOLS "Build FBX content cooker and content-dependent tests" ON)
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

if(HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42F6E95CAD6EC532FD372391373363B62A14AF6D771056DBFC86160E6DFFF7AA
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

if(HS_BUILD_CONTENT_TOOLS)
    set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        directxtex
        GIT_REPOSITORY https://github.com/microsoft/DirectXTex.git
        GIT_TAG 4feb3e1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(directxtex)
endif()

if(HS_BUILD_RUNTIME)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.9b
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(imgui)

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
endif()

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
    Source/Core/Private/cooked_format.cpp
    Source/Core/Private/fixed_step_clock.cpp
    Source/Core/Private/process_info.cpp
    Source/Core/Private/render_snapshot.cpp
    Source/Core/Private/snapshot_exchange.cpp
)
target_include_directories(hs_core PUBLIC Source/Core/Public)
target_link_libraries(hs_core PRIVATE psapi)
target_compile_definitions(hs_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_core)

add_library(
    hs_jobs STATIC
    Source/Jobs/Private/task_system.cpp
)
target_include_directories(hs_jobs PUBLIC Source/Jobs/Public)
target_link_libraries(hs_jobs PUBLIC hs_core PRIVATE Taskflow::Taskflow)
target_compile_definitions(hs_jobs PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_jobs)

add_library(hs_game_domain STATIC Source/GameDomain/Private/game_read_model.cpp)
target_include_directories(hs_game_domain PUBLIC Source/GameDomain/Public)
target_link_libraries(hs_game_domain PUBLIC hs_core)
hs_configure_target(hs_game_domain)

add_library(
    hs_presentation STATIC
    Source/Presentation/Private/presentation_catalog.cpp
    Source/Presentation/Private/presentation_event_projector.cpp
    Source/Presentation/Private/snapshot_projector.cpp
    Source/Presentation/Private/ui_interaction.cpp
)
target_include_directories(hs_presentation PUBLIC Source/Presentation/Public)
target_link_libraries(hs_presentation PUBLIC hs_core hs_game_domain)
hs_configure_target(hs_presentation)

add_library(
    hs_game_rules STATIC
    Source/GameRules/Private/simulation_rules.cpp
)
target_include_directories(hs_game_rules PUBLIC Source/GameRules/Public)
target_link_libraries(hs_game_rules PUBLIC hs_core hs_game_domain)
hs_configure_target(hs_game_rules)

add_library(
    hs_gameplay STATIC
    Source/Gameplay/Private/game_simulation.cpp
    Source/Gameplay/Private/simulation_combat.cpp
    Source/Gameplay/Private/simulation_progression.cpp
    Source/Gameplay/Private/simulation_tick.cpp
)
target_include_directories(hs_gameplay PUBLIC Source/Gameplay/Public)
target_link_libraries(hs_gameplay PUBLIC hs_core hs_game_domain hs_game_rules)
if(MSVC)
    target_compile_options(hs_gameplay PRIVATE /fp:strict)
endif()
hs_configure_target(hs_gameplay)

if(HS_BUILD_RUNTIME)
add_library(
    hs_renderer_d3d12 STATIC
    Source/RendererD3D12/Private/render_graph.cpp
    Source/RendererD3D12/Private/renderer_diagnostics.cpp
    Source/RendererD3D12/Private/renderer.cpp
    Source/RendererD3D12/Private/renderer_capture.cpp
    Source/RendererD3D12/Private/renderer_device.cpp
    Source/RendererD3D12/Private/renderer_frame.cpp
    Source/RendererD3D12/Private/renderer_pipeline.cpp
    Source/RendererD3D12/Private/renderer_resources.cpp
    Source/RendererD3D12/Private/renderer_environment.cpp
    Source/RendererD3D12/Private/renderer_environment_mesh.cpp
    Source/RendererD3D12/Private/renderer_ui.cpp
)
target_sources(
    hs_renderer_d3d12 PRIVATE
    $<$<CONFIG:Debug>:${imgui_SOURCE_DIR}/imgui.cpp>
    $<$<CONFIG:Debug>:${imgui_SOURCE_DIR}/imgui_draw.cpp>
    $<$<CONFIG:Debug>:${imgui_SOURCE_DIR}/imgui_tables.cpp>
    $<$<CONFIG:Debug>:${imgui_SOURCE_DIR}/imgui_widgets.cpp>
    $<$<CONFIG:Debug>:${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp>
    $<$<CONFIG:Debug>:${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp>
)
target_include_directories(hs_renderer_d3d12 PUBLIC Source/RendererD3D12/Public)
target_include_directories(
    hs_renderer_d3d12
    BEFORE PRIVATE "${agility_SOURCE_DIR}/build/native/include"
)
target_include_directories(
    hs_renderer_d3d12 PRIVATE "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends"
)
target_link_libraries(
    hs_renderer_d3d12
    PUBLIC hs_core
    PRIVATE D3D12MemoryAllocator WinPixEventRuntime d3d12 dxgi dxguid windowscodecs gdi32
)
target_compile_definitions(hs_renderer_d3d12 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(
    hs_renderer_d3d12 PRIVATE $<$<CONFIG:Debug>:HS_DEVELOPMENT_TOOLS=1>
)
if(HS_ENABLE_VALIDATION)
    target_compile_definitions(hs_renderer_d3d12 PRIVATE USE_PIX=1)
else()
    target_compile_definitions(hs_renderer_d3d12 PRIVATE USE_PIX_RETAIL=1)
endif()
hs_configure_target(hs_renderer_d3d12)

add_library(
    hs_runtime STATIC
    Source/Runtime/Private/application.cpp
    Source/Runtime/Private/audio_engine.cpp
    Source/Runtime/Private/experiment_spec.cpp
    Source/Runtime/Private/playtest_recording.cpp
    Source/Runtime/Private/save_store.cpp
    Source/Runtime/Private/vfx_catalog.cpp
    Source/Runtime/Private/window.cpp
)
target_sources(
    hs_runtime PRIVATE
    $<$<NOT:$<CONFIG:Release>>:${CMAKE_CURRENT_SOURCE_DIR}/Source/Runtime/Private/experiment_pipe.cpp>
)
target_include_directories(hs_runtime PUBLIC Source/Runtime/Public)
target_link_libraries(
    hs_runtime
    PUBLIC hs_core hs_gameplay
    PRIVATE hs_jobs hs_presentation hs_renderer_d3d12 nlohmann_json::nlohmann_json
            user32 shell32 xaudio2 ole32
)
target_compile_definitions(hs_runtime PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_definitions(
    hs_runtime PRIVATE $<$<NOT:$<CONFIG:Release>>:HS_DEVELOPMENT_TOOLS=1>
)
hs_configure_target(hs_runtime)
endif()

if(HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS)
find_program(
    HS_DXC
    NAMES dxc.exe
    REQUIRED
    HINTS
        "$ENV{WindowsSdkBinPath}/x64"
)
set(HS_SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/project_hs.hlsl")
set(HS_SHADER_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Generated/Shaders")
file(MAKE_DIRECTORY "${HS_SHADER_DIRECTORY}")

function(hs_compile_shader output entry profile)
    add_custom_command(
        OUTPUT "${HS_SHADER_DIRECTORY}/${output}"
        COMMAND
            "${HS_DXC}" -nologo -WX -O3 -Ges -E "${entry}" -T "${profile}"
            -Fo "${HS_SHADER_DIRECTORY}/${output}" "${HS_SHADER_SOURCE}"
        DEPENDS "${HS_SHADER_SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/environment.hlsli"
        VERBATIM
    )
    set(HS_SHADER_OUTPUTS ${HS_SHADER_OUTPUTS} "${HS_SHADER_DIRECTORY}/${output}" PARENT_SCOPE)
endfunction()

hs_compile_shader(scene_vs.dxil SceneVS vs_6_0)
hs_compile_shader(scene_ps.dxil ScenePS ps_6_0)
hs_compile_shader(slime_ps.dxil SlimePS ps_6_6)
hs_compile_shader(shadow_vs.dxil ShadowVS vs_6_0)
hs_compile_shader(shadow_ps.dxil ShadowPS ps_6_0)
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
endif()

if(HS_BUILD_RUNTIME)
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
target_link_libraries(
    hs_experiment PRIVATE hs_runtime hs_gameplay hs_presentation nlohmann_json::nlohmann_json dbghelp
)
target_compile_definitions(
    hs_experiment PRIVATE HS_ENABLE_VALIDATION=1 WIN32_LEAN_AND_MEAN NOMINMAX
                          $<$<NOT:$<CONFIG:Release>>:HS_DEVELOPMENT_TOOLS=1>
)
hs_configure_target(hs_experiment)
add_dependencies(hs_experiment hs_runtime_assets)
endif()

if(HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS)
    add_executable(hs_combat_sim Source/Apps/CombatSim/main.cpp)
    target_link_libraries(
        hs_combat_sim PRIVATE hs_gameplay hs_presentation nlohmann_json::nlohmann_json
    )
    target_compile_definitions(hs_combat_sim PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    hs_configure_target(hs_combat_sim)
endif()

if(HS_BUILD_CONTENT_TOOLS)
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

add_executable(
    hs_content
    Source/Apps/Content/main.cpp
    Source/Apps/Content/character_asset_cooker.cpp
    Source/Apps/Content/environment_texture_cooker.cpp
    Source/Apps/Content/environment_histogram_cooker.cpp
    Source/Apps/Content/environment_mesh_cooker.cpp
    Source/Apps/Content/content_cooker.cpp
    Source/Apps/Content/content_source_inventory.cpp
    Source/Apps/Content/game_data_cooker.cpp
    Source/Apps/Content/game_data_validation.cpp
    Source/Apps/Content/particle_effect_cooker.cpp
)
target_link_libraries(
    hs_content
    PRIVATE hs_core hs_game_rules hs_presentation nlohmann_json::nlohmann_json
            DirectXTex fbxsdk fbx_xml fbx_zlib ws2_32 windowscodecs d3d11
)
add_dependencies(hs_content hs_shaders)
target_compile_definitions(
    hs_content
    PRIVATE
        HS_COOKED_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}/Cooked"
        HS_SHADER_DIRECTORY="${HS_SHADER_DIRECTORY}"
        HS_GAME_DATA_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/GameData"
        HS_AUDIO_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Audio"
        HS_VFX_TEXTURE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/VFX"
        HS_SCHEMA_FILE="${CMAKE_CURRENT_SOURCE_DIR}/Schemas/game.schema.json"
        HS_CHARACTER_MODEL="${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Characters/Archer/ErikaArcher.fbx"
        HS_CHARACTER_ANIMATION_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Characters/Archer"
        HS_MONSTER_MODEL_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Monsters"
        HS_MONSTER_ANIMATION_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters"
        HS_MONSTER_TEXTURE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/Monsters/PBR"
)
target_include_directories(hs_content SYSTEM PRIVATE "${HS_FBX_SDK_ROOT}/include")
hs_configure_target(hs_content)

file(GLOB HS_GAME_DATA_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/GameData/*.json"
)
file(GLOB HS_AUDIO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Audio/*.wav"
)
set(
    HS_ARCHER_ASSET_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Characters/Archer/ErikaArcher.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Characters/Archer/Idle.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Characters/Archer/RunForward.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Characters/Archer/DrawArrow.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Characters/Archer/AimRecoil.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Characters/Archer/DeathBackward.fbx"
)
file(GLOB_RECURSE HS_ENVIRONMENT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/Environment/*.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/Environment/*.json"
)
file(GLOB HS_VFX_TEXTURE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/VFX/*.png"
)
file(GLOB HS_ARCHER_TEXTURE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Characters/Archer/ErikaArcher.fbm/*"
)
list(APPEND HS_ARCHER_ASSET_SOURCES ${HS_ARCHER_TEXTURE_SOURCES})
file(GLOB HS_MONSTER_TEXTURE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/Monsters/PBR/*.png"
)
set(
    HS_MONSTER_ASSET_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Monsters/TurtleShell_SK.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Monsters/ChestMonster_SK.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Monsters/Beholder_SK.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/5m/IdleBattle.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/5m/Run.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/5m/Attack01.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/5m/Attack02.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/5m/Die.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/10m/IdleBattle.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/10m/Run.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/10m/Attack01.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/10m/Attack02.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/10m/Die.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/final/IdleBattle.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/final/Run.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/final/Attack01.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/final/Attack03.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/final/Die.fbx"
)
foreach(variant Melee Ranged Suicide Projectile)
    if(variant STREQUAL "Projectile")
        set(stem SlimeGelProjectile)
    else()
        set(stem Slime${variant})
        foreach(clip Idle Run Draw Recoil Death)
            list(APPEND HS_MONSTER_ASSET_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters/SlimeFamily/${variant}/${clip}.fbx")
        endforeach()
    endif()
    list(APPEND HS_MONSTER_ASSET_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Monsters/SlimeFamily/${variant}/${stem}.fbx"
        "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Monsters/SlimeFamily/${variant}/${stem}.fbm/${stem}_BaseColor.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Monsters/SlimeFamily/${variant}/${stem}.fbm/${stem}_Normal.png")
endforeach()
set(HS_ENVIRONMENT_COOKED
    "${CMAKE_CURRENT_BINARY_DIR}/Cooked/environment_terrain_gaussian.dds"
    "${CMAKE_CURRENT_BINARY_DIR}/Cooked/environment_terrain_histogram.dds")
foreach(group terrain rock bark grass leaf)
    if(group STREQUAL "terrain")
        set(roles basecolor surface normal detail height)
    elseif(group STREQUAL "rock" OR group STREQUAL "bark")
        set(roles basecolor surface normal detail)
    else()
        set(roles basecolor normal roughness)
    endif()
    foreach(role IN LISTS roles)
        list(APPEND HS_ENVIRONMENT_COOKED "${CMAKE_CURRENT_BINARY_DIR}/Cooked/environment_${group}_${role}.dds")
    endforeach()
endforeach()
foreach(group trunk leaf rock grass)
    if(group STREQUAL "trunk" OR group STREQUAL "leaf")
        set(last_variant 2)
    else()
        set(last_variant 3)
    endif()
    foreach(variant RANGE 0 ${last_variant})
        foreach(suffix "" _lod1 _lod2)
            list(APPEND HS_ENVIRONMENT_COOKED "${CMAKE_CURRENT_BINARY_DIR}/Cooked/environment_${group}_${variant}${suffix}.meshbin")
        endforeach()
    endforeach()
endforeach()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Cooked/manifest.txt"
    COMMAND "$<TARGET_FILE:hs_content>" --cook
    DEPENDS hs_content hs_shaders "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Environment/environment_meshes.json" ${HS_ENVIRONMENT_SOURCES} ${HS_GAME_DATA_SOURCES} ${HS_AUDIO_SOURCES} ${HS_VFX_TEXTURE_SOURCES} ${HS_ARCHER_ASSET_SOURCES} ${HS_MONSTER_ASSET_SOURCES} ${HS_MONSTER_TEXTURE_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/game.schema.json"
    BYPRODUCTS
        ${HS_ENVIRONMENT_COOKED}
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/archer.meshbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/enemy_melee.meshbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/enemy_melee_diffuse_0.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/enemy_melee_normal_0.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/enemy_ranged.meshbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/enemy_suicide.meshbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/enemy_ranged_diffuse_0.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/enemy_ranged_normal_0.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/enemy_suicide_diffuse_0.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/enemy_suicide_normal_0.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/enemy_gel_projectile.meshbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/enemy_gel_projectile_diffuse_0.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/enemy_gel_projectile_normal_0.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/boss_5m.meshbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/boss_10m.meshbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/boss_final.meshbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/archer_diffuse_0.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/archer_diffuse_1.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/archer_diffuse_2.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/archer_diffuse_3.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/archer_diffuse_4.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/archer_diffuse_5.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/archer_normal_0.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/archer_normal_1.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/archer_normal_2.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/archer_normal_3.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/archer_normal_4.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/archer_normal_5.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/monster_basecolor.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/monster_emissive.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/monster_ram.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/simulation_rules.hsbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/presentation_catalog.hsbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/particle_effects.hsbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/vfx_masks.dds"
    VERBATIM
)
add_custom_target(
    hs_cooked_content ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/Cooked/manifest.txt" ${HS_ENVIRONMENT_COOKED}
)
add_dependencies(hs_combat_sim hs_cooked_content)
if(HS_BUILD_RUNTIME)
add_dependencies(ProjectHS hs_cooked_content)
add_dependencies(hs_experiment hs_cooked_content)

add_executable(
    hs_content_tools
    Source/Apps/ContentTools/main.cpp
    Source/Apps/Content/content_source_inventory.cpp
)
target_link_libraries(hs_content_tools PRIVATE hs_core nlohmann_json::nlohmann_json)
target_compile_definitions(
    hs_content_tools
    PRIVATE
        HS_GAME_DATA_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/GameData"
        HS_AUDIO_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Audio"
        HS_CHARACTER_MODEL="${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Characters/Archer/ErikaArcher.fbx"
        HS_CHARACTER_ANIMATION_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Characters/Archer"
        HS_MONSTER_MODEL_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Monsters"
        HS_MONSTER_ANIMATION_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Monsters"
        HS_MONSTER_TEXTURE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/Monsters/PBR"
        HS_CONTENT_EXECUTABLE="$<TARGET_FILE:hs_content>"
        HS_PROJECT_EXECUTABLE="$<TARGET_FILE:ProjectHS>"
        HS_COOKED_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}/Cooked"
        HS_CMAKE_EXECUTABLE="${CMAKE_COMMAND}"
        HS_BINARY_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}"
)
add_dependencies(hs_content_tools hs_content ProjectHS)
hs_configure_target(hs_content_tools)
endif()
endif()

if(HS_BUILD_TESTS)
    enable_testing()
    add_executable(hs_tests Tests/foundation_tests.cpp)
    target_link_libraries(hs_tests PRIVATE hs_core hs_jobs)
    hs_configure_target(hs_tests)

    add_test(NAME foundation.core COMMAND hs_tests foundation)
    add_test(NAME foundation.task-system COMMAND hs_tests jobs)
    add_test(
        NAME architecture.layers
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_architecture.cmake
    )
    if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
        add_test(
            NAME build.msvc-header-dependencies
            COMMAND ${CMAKE_COMMAND}
                -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
                -DBINARY_DIR=${CMAKE_BINARY_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_msvc_dependencies.cmake
        )
    endif()
    add_executable(hs_gameplay_internal_tests Tests/gameplay_internal_tests.cpp)
    target_link_libraries(hs_gameplay_internal_tests PRIVATE hs_game_domain)
    target_include_directories(hs_gameplay_internal_tests PRIVATE Source/Gameplay/Private)
    hs_configure_target(hs_gameplay_internal_tests)
    add_test(NAME simulation.internal-invariants COMMAND hs_gameplay_internal_tests)
    add_executable(hs_navigation_tests Tests/gameplay_navigation_tests.cpp)
    target_link_libraries(hs_navigation_tests PRIVATE hs_game_domain)
    target_include_directories(hs_navigation_tests PRIVATE Source/Gameplay/Private)
    hs_configure_target(hs_navigation_tests)
    add_test(NAME simulation.navigation COMMAND hs_navigation_tests)
    add_executable(hs_simulation_tests Tests/simulation_tests.cpp)
    target_link_libraries(hs_simulation_tests PRIVATE hs_core hs_game_rules hs_gameplay)
    hs_configure_target(hs_simulation_tests)
    add_test(NAME simulation.determinism-core COMMAND hs_simulation_tests)
    if(HS_BUILD_CONTENT_TOOLS)
    add_test(NAME content.validate COMMAND hs_content --validate-only)
    if(TARGET hs_content_tools)
    add_test(NAME tools.content-status COMMAND hs_content_tools status)
    endif()
    endif()
    if(HS_BUILD_RUNTIME)
    add_test(
        NAME integration.render-smoke
        COMMAND
            ProjectHS --smoke --ticks=180 --barriers=legacy
            --resize-test
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/render-smoke
    )
    set_tests_properties(integration.render-smoke PROPERTIES TIMEOUT 120)
    add_test(
        NAME vfx.showcase
        COMMAND
            ProjectHS --smoke --vfx-showcase --ticks=60 --barriers=legacy
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/vfx-showcase
    )
    set_tests_properties(vfx.showcase PROPERTIES TIMEOUT 120)
    add_test(
        NAME rendering.barriers
        COMMAND
            powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Tests/compare_barriers.ps1"
            -Executable "$<TARGET_FILE:ProjectHS>"
            -Artifacts "${CMAKE_CURRENT_BINARY_DIR}/Artifacts/barriers"
    )
    set_tests_properties(rendering.barriers PROPERTIES TIMEOUT 120)
    add_test(
        NAME experiment.offscreen
        COMMAND
            hs_experiment
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/experiment
    )
    set_tests_properties(experiment.offscreen PROPERTIES TIMEOUT 120)
    endif()

    if(HS_BUILD_CONTENT_TOOLS)
    add_executable(
        hs_gameplay_tests
        Tests/gameplay_tests.cpp
        Tests/gameplay_test_support.cpp
        Tests/gameplay_content_tests.cpp
        Tests/gameplay_combat_tests.cpp
        Tests/gameplay_skill_tests.cpp
        Tests/gameplay_upgrade_tests.cpp
        Tests/gameplay_relic_tests.cpp
        Tests/gameplay_boss_tests.cpp
        Tests/gameplay_progression_tests.cpp
        Tests/gameplay_presentation_tests.cpp
        Tests/gameplay_determinism_tests.cpp
    )
    target_link_libraries(hs_gameplay_tests PRIVATE hs_core hs_gameplay hs_presentation)
    hs_configure_target(hs_gameplay_tests)
    add_dependencies(hs_gameplay_tests hs_cooked_content)
    add_test(NAME content.environment-assets COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_environment_assets.ps1"
        -Cooked "${CMAKE_CURRENT_BINARY_DIR}/Cooked")
    add_test(NAME simulation.content COMMAND hs_gameplay_tests content)
    add_test(NAME simulation.combat COMMAND hs_gameplay_tests combat)
    add_test(NAME simulation.progression COMMAND hs_gameplay_tests progression)
    add_test(NAME simulation.presentation-contract COMMAND hs_gameplay_tests presentation)
    add_test(NAME simulation.determinism COMMAND hs_gameplay_tests determinism)
    set_tests_properties(
        simulation.content
        simulation.combat
        simulation.progression
        simulation.presentation-contract
        simulation.determinism
        PROPERTIES TIMEOUT 120
    )
    endif()
    if(TARGET hs_combat_sim)
    add_test(
        NAME scenario.combat-simulator
        COMMAND hs_combat_sim
            --suite=${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data/combat_suite_smoke.json
            --output=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/combat-simulator
    )
    set_tests_properties(scenario.combat-simulator PROPERTIES TIMEOUT 30)
    add_test(
        NAME tools.combat-suite-input
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_combat_suite_input.ps1
            -Binary $<TARGET_FILE:hs_combat_sim>
            -SourceRoot ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(tools.combat-suite-input PROPERTIES TIMEOUT 30)
    endif()

    if(HS_BUILD_RUNTIME)
    add_executable(hs_runtime_tests Tests/runtime_tests.cpp)
    target_link_libraries(hs_runtime_tests PRIVATE hs_runtime)
    target_include_directories(hs_runtime_tests PRIVATE
        Source/Runtime/Private
        Source/RendererD3D12/Public
    )
    hs_configure_target(hs_runtime_tests)
    add_test(NAME runtime.integration COMMAND hs_runtime_tests)
    set_tests_properties(runtime.integration PROPERTIES TIMEOUT 30)
    add_test(
        NAME experiment.simulation
        COMMAND
            hs_experiment --simulation-only
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/experiment-simulation
    )
    set_tests_properties(experiment.simulation PROPERTIES TIMEOUT 30)
    add_test(
        NAME experiment.pipe
        COMMAND
            hs_experiment --simulation-only --timeline-self-test
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/experiment-pipe
    )
    set_tests_properties(experiment.pipe PROPERTIES TIMEOUT 30)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_test(
            NAME rendering.shader-hot-reload
            COMMAND
                powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/Tests/shader_hot_reload.ps1"
                -Executable "$<TARGET_FILE:ProjectHS>"
                -Shader "${CMAKE_CURRENT_BINARY_DIR}/Shaders/scene_ps.dxil"
                -Artifacts "${CMAKE_CURRENT_BINARY_DIR}/Artifacts/shader-hot-reload"
        )
        set_tests_properties(rendering.shader-hot-reload PROPERTIES TIMEOUT 45)
    endif()
    endif()
endif()

if(HS_BUILD_RUNTIME)
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
if(HS_BUILD_CONTENT_TOOLS)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/archer.meshbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/simulation_rules.hsbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/presentation_catalog.hsbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/particle_effects.hsbin"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/vfx_masks.dds"
        "${CMAKE_CURRENT_BINARY_DIR}/Cooked/manifest.txt"
    DESTINATION Cooked
    COMPONENT Runtime
)
install(
    DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Cooked/"
    DESTINATION Cooked
    COMPONENT Runtime
    FILES_MATCHING PATTERN "archer_*.dds"
)
install(
    DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Cooked/Audio/"
    DESTINATION Cooked/Audio
    COMPONENT Runtime
    FILES_MATCHING PATTERN "*.json" PATTERN "*.wav"
)
endif()
install(FILES ThirdPartyNotices.txt DESTINATION . COMPONENT Runtime)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME ProjectHS)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(
    CPACK_INSTALL_CMAKE_PROJECTS
    "${CMAKE_BINARY_DIR};${PROJECT_NAME};Runtime;/"
)
include(CPack)
endif()
