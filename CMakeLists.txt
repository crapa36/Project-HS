cmake_minimum_required(VERSION 4.4)

project(ProjectHS VERSION 0.2.0 LANGUAGES C CXX)

# Single runtime pack location, shared by linker and installer (not a cache option).
set(HS_SHIPPING_PACK_RELATIVE_PATH "Content/content.hsp")

include(FetchContent)

if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    # Project the object-owned JSON header closure into Ninja's MSVC protocol.
    # Localized /showIncludes text is not a reliable dependency interchange.
    set(HS_MSVC_INCLUDE_PROJECTOR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MsvcIncludeProjection.cmake")
    file(SHA256 "${HS_MSVC_INCLUDE_PROJECTOR}" HS_MSVC_INCLUDE_PROJECTOR_ID)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HS_MSVC_INCLUDE_PROJECTOR}")
    foreach(language IN ITEMS C CXX)
        set(CMAKE_${language}_COMPILER_LAUNCHER
            "${CMAKE_COMMAND}" "-DHS_INCLUDE_PROJECTOR_ID=${HS_MSVC_INCLUDE_PROJECTOR_ID}"
            -P "${HS_MSVC_INCLUDE_PROJECTOR}" -- ${CMAKE_${language}_COMPILER_LAUNCHER})
        set(CMAKE_${language}_CL_SHOWINCLUDES_PREFIX "Note: including file: ")
    endforeach()
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file: ")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(HS_BUILD_TESTS "Build Project HS tests" ON)
option(HS_BUILD_RENDER_DATA_TESTS "Build CPU render package tests without content tools" OFF)
option(HS_BUILD_ASSET_PIPELINE_TESTS "Build portable asset producer and generation tests" OFF)
option(HS_BUILD_RUNTIME "Build the D3D12 runtime and applications" ON)
option(HS_BUILD_CONTENT_TOOLS "Build FBX content cooker and content-dependent tests" ON)
option(HS_WITH_BRIXELIZER "Build the pinned native FidelityFX Brixelizer/GI integration" OFF)
option(HS_WITH_DIRECTSTORAGE "Build the pinned native DirectStorage 1.3.0 asset IO path" OFF)
option(HS_ENABLE_VALIDATION "Enable D3D12 validation" OFF)
option(HS_ENABLE_GPU_VALIDATION "Enable D3D12 GPU-based validation" OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set(HS_SINGLE_CONFIG_RELEASE ON)
endif()
if(HS_BUILD_RUNTIME AND NOT HS_BUILD_CONTENT_TOOLS AND HS_SINGLE_CONFIG_RELEASE)
    foreach(shipping_input IN ITEMS
            "Shipping/content.generated.cpp" "${HS_SHIPPING_PACK_RELATIVE_PATH}")
        if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/${shipping_input}")
            message(FATAL_ERROR
                "Prebuilt Release requires ${shipping_input} in ${CMAKE_CURRENT_BINARY_DIR}; build with HS_BUILD_CONTENT_TOOLS=ON to produce the shipping pair first.")
        endif()
    endforeach()
endif()
if(HS_BUILD_RUNTIME)
    install(SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_release_begin.cmake"
        CONFIGURATIONS Release COMPONENT Runtime)
endif()

if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    # Each completed object owns its dependency snapshot. Querying Ninja's
    # shared writable deps log during a parallel build races Windows file locks.
    string(APPEND CMAKE_C_COMPILE_OBJECT " /sourceDependencies <OBJECT>.hsdeps.json")
    string(APPEND CMAKE_CXX_COMPILE_OBJECT " /sourceDependencies <OBJECT>.hsdeps.json")
endif()

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
# Reproducible correction of the pinned backend, including existing source overrides.
# Hash verification rejects unsupported or locally modified Taskflow sources.
include(cmake/patches/taskflow-async-admission.cmake)
hs_patch_taskflow("${taskflow_SOURCE_DIR}")

if(HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS OR HS_BUILD_RENDER_DATA_TESTS OR HS_BUILD_ASSET_PIPELINE_TESTS)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42F6E95CAD6EC532FD372391373363B62A14AF6D771056DBFC86160E6DFFF7AA
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

if(HS_BUILD_CONTENT_TOOLS OR HS_BUILD_RUNTIME)
    set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        directxtex
        GIT_REPOSITORY https://github.com/microsoft/DirectXTex.git
        GIT_TAG 4feb3e1
    )
    FetchContent_MakeAvailable(directxtex)
endif()

if(HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS)
    set(HS_FONT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Generated/Fonts")
    file(MAKE_DIRECTORY "${HS_FONT_DIRECTORY}")
    if(HS_BUILD_CONTENT_TOOLS OR NOT HS_SINGLE_CONFIG_RELEASE)
        file(
            DOWNLOAD
            "https://raw.githubusercontent.com/google/fonts/7ff85c87f93ea6cca5f41c69f2e4edcb90240f26/ofl/notosanskr/NotoSansKR%5Bwght%5D.ttf"
            "${HS_FONT_DIRECTORY}/NotoSansKR.ttf"
            EXPECTED_HASH SHA256=194018E6B2B293A7964F037B25C0249CE1418BC9AB3C971060A03AA57861E252
            TLS_VERIFY ON
        )
    endif()
    file(
        DOWNLOAD
        "https://raw.githubusercontent.com/google/fonts/7ff85c87f93ea6cca5f41c69f2e4edcb90240f26/ofl/notosanskr/OFL.txt"
        "${HS_FONT_DIRECTORY}/NotoSansKR-OFL.txt"
        EXPECTED_HASH SHA256=1C05C68C34F9708415AADA51F17E1B0092D2CEA709BF4A94CD38114F9E73D7D9
        TLS_VERIFY ON
    )

endif()

if(HS_BUILD_RUNTIME)
    if(NOT HS_SINGLE_CONFIG_RELEASE)
        FetchContent_Declare(
            imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG v1.91.9b
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(imgui)
    endif()

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

    set(D3D12MA_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
    set(D3D12MA_AGILITY_SDK_DIRECTORY "${agility_SOURCE_DIR}" CACHE STRING "" FORCE)
    FetchContent_Declare(
        d3d12ma
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.git
        GIT_TAG 1d86c1130f61453634b1df85782e1fecfd59a525
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
    Source/Core/Private/graphics_settings.cpp
    Source/Core/Private/sha256.cpp
    Source/Core/Private/shipped_content_owner.cpp
    Source/Core/Private/process_info.cpp
)
target_include_directories(hs_core PUBLIC Source/Core/Public)
if(WIN32)
    target_link_libraries(hs_core PRIVATE psapi bcrypt)
endif()
target_compile_definitions(hs_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_core)

add_library(hs_render_data_values STATIC Source/RenderData/Private/render_snapshot.cpp)
target_include_directories(hs_render_data_values PUBLIC Source/RenderData/Public)
target_link_libraries(hs_render_data_values PUBLIC hs_core)
hs_configure_target(hs_render_data_values)

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

add_library(hs_environment_layout STATIC
    Source/Presentation/Private/environment_layout.cpp)
target_include_directories(hs_environment_layout PUBLIC Source/Presentation/Public)
target_link_libraries(hs_environment_layout PUBLIC hs_core hs_game_domain hs_render_data_values)
hs_configure_target(hs_environment_layout)

add_library(
    hs_presentation STATIC
    Source/Presentation/Private/presentation_event_projector.cpp
    Source/Presentation/Private/environment_projector.cpp
    Source/Presentation/Private/snapshot_projector.cpp
    Source/Presentation/Private/ui_interaction.cpp
    Source/Presentation/Private/snapshot_exchange.cpp
)
target_include_directories(hs_presentation PUBLIC Source/Presentation/Public)
target_link_libraries(hs_presentation PUBLIC hs_core hs_game_domain hs_environment_layout hs_render_data_values)
hs_configure_target(hs_presentation)

# File decoding belongs to developer tools; shipped tables are compiled constants.
add_library(hs_presentation_io STATIC EXCLUDE_FROM_ALL Source/Presentation/Private/presentation_catalog.cpp)
target_link_libraries(hs_presentation_io PUBLIC hs_presentation)
hs_configure_target(hs_presentation_io)

add_library(
    hs_game_rules STATIC
    Source/GameRules/Private/simulation_rules.cpp
)
target_include_directories(hs_game_rules PUBLIC Source/GameRules/Public)
target_link_libraries(hs_game_rules PUBLIC hs_core hs_game_domain)
hs_configure_target(hs_game_rules)
add_library(hs_game_rules_io STATIC EXCLUDE_FROM_ALL Source/GameRules/Private/simulation_rules_io.cpp)
target_link_libraries(hs_game_rules_io PUBLIC hs_game_rules)
hs_configure_target(hs_game_rules_io)

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
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hs_gameplay PRIVATE -fno-fast-math -ffp-contract=off -frounding-math)
endif()
hs_configure_target(hs_gameplay)

add_library(hs_gameplay_development STATIC EXCLUDE_FROM_ALL
    Source/Gameplay/Private/game_simulation_development.cpp)
target_link_libraries(hs_gameplay_development PUBLIC hs_gameplay)
hs_configure_target(hs_gameplay_development)

if(HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS OR HS_BUILD_RENDER_DATA_TESTS OR HS_BUILD_ASSET_PIPELINE_TESTS)
add_library(hs_generation_lock STATIC
    Source/AssetManifest/Private/generation_lock.cpp)
target_include_directories(hs_generation_lock PUBLIC Source/AssetManifest/Public)
hs_configure_target(hs_generation_lock)

add_library(hs_asset_manifest STATIC
    Source/AssetManifest/Private/selected_generation.cpp)
target_include_directories(hs_asset_manifest PUBLIC Source/AssetManifest/Public)
target_link_libraries(hs_asset_manifest PUBLIC hs_core hs_generation_lock PRIVATE nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(hs_asset_manifest PRIVATE bcrypt)
endif()
hs_configure_target(hs_asset_manifest)
endif()

# Public value/contract headers are usable without the native Runtime backend.
add_library(hs_runtime_contracts INTERFACE)
target_include_directories(hs_runtime_contracts INTERFACE Source/Runtime/Public Source/RendererD3D12/Public)
target_link_libraries(hs_runtime_contracts INTERFACE hs_core hs_game_domain hs_render_data_values)

if(HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS)
add_library(hs_vfx_program_io STATIC EXCLUDE_FROM_ALL Source/RendererD3D12/Private/vfx_program_loader.cpp)
target_include_directories(hs_vfx_program_io PUBLIC Source/RendererD3D12/Public)
target_link_libraries(hs_vfx_program_io PUBLIC hs_core hs_render_data_values)
hs_configure_target(hs_vfx_program_io)
add_library(hs_vfx_image_io STATIC EXCLUDE_FROM_ALL Source/Runtime/Private/vfx_image_io.cpp
    Source/Runtime/Private/vfx_image_identity.cpp)
target_link_libraries(hs_vfx_image_io PUBLIC hs_runtime_contracts hs_vfx_program_io)
hs_configure_target(hs_vfx_image_io)
endif()

if(HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS OR HS_BUILD_RENDER_DATA_TESTS)
add_library(hs_meshoptimizer_codec STATIC ThirdParty/meshoptimizer/src/vertexcodec.cpp)
target_include_directories(hs_meshoptimizer_codec PUBLIC ThirdParty/meshoptimizer/src)
hs_configure_target(hs_meshoptimizer_codec)
if(MSVC)
    set_property(TARGET hs_meshoptimizer_codec PROPERTY MSVC_RUNTIME_CHECKS "")
    target_compile_options(hs_meshoptimizer_codec PRIVATE "$<$<CONFIG:Debug>:/O2;/Ob2>")
endif()
add_library(hs_render_data STATIC
    Source/RenderData/Private/render_asset_format.cpp
    Source/RenderData/Private/material_schema.cpp
    Source/RenderData/Private/material_descriptor.cpp
    Source/RenderData/Private/character_descriptor.cpp
    Source/RenderData/Private/render_revision.cpp
    Source/RenderData/Private/material_sidecar.cpp
    Source/RenderData/Private/geometry_hierarchy.cpp
    Source/RenderData/Private/scene_hlod.cpp
    Source/RenderData/Private/scene_probe.cpp
    Source/RenderData/Private/static_shadow.cpp
    Source/RenderData/Private/static_shadow_archive.cpp
    Source/RenderData/Private/volumetric_medium.cpp
    Source/RenderData/Private/quality_profiles.cpp
    Source/RenderData/Private/character_animation_extension.cpp
    Source/RenderData/Private/triangle_sample.cpp
    Source/RenderData/Private/character_package.cpp
    Source/RenderData/Private/geometry_package.cpp
    Source/RenderData/Private/lighting_probe.cpp
    Source/RenderData/Private/ltc_lut.cpp
    Source/RenderData/Private/texture_package.cpp
    Source/RenderData/Private/volume_texture_package.cpp)
target_include_directories(hs_render_data PUBLIC Source/RenderData/Public)
target_link_libraries(hs_render_data PUBLIC hs_core hs_render_data_values)
target_link_libraries(hs_render_data PRIVATE hs_meshoptimizer_codec)
hs_configure_target(hs_render_data)
endif()

if(HS_BUILD_CONTENT_TOOLS OR HS_BUILD_RENDER_DATA_TESTS)
add_library(hs_material_compiler STATIC Source/RenderData/Private/material_ir.cpp)
target_link_libraries(hs_material_compiler PUBLIC hs_render_data)
hs_configure_target(hs_material_compiler)

add_executable(hs_character_interchange
    Source/Apps/CharacterInterchange/main.cpp
    Source/Apps/CharacterInterchange/character_interchange.cpp)
target_link_libraries(hs_character_interchange PRIVATE
    hs_render_data nlohmann_json::nlohmann_json)
hs_configure_target(hs_character_interchange)
endif()

if(HS_BUILD_CONTENT_TOOLS OR HS_BUILD_ASSET_PIPELINE_TESTS)
add_library(hs_asset_pipeline STATIC Source/Apps/AssetPipeline/action.cpp)
target_link_libraries(hs_asset_pipeline PRIVATE hs_core hs_generation_lock nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(hs_asset_pipeline PRIVATE bcrypt)
endif()
hs_configure_target(hs_asset_pipeline)

add_executable(hs_audio_content
    Source/Apps/AudioContent/main.cpp
    Source/Apps/Content/release_audio_codegen.cpp)

target_include_directories(hs_audio_content PRIVATE Source/Apps/Content)
target_link_libraries(hs_audio_content PRIVATE hs_runtime_contracts)
target_link_libraries(hs_audio_content PRIVATE hs_asset_pipeline)
target_link_libraries(hs_audio_content PRIVATE hs_core nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(hs_audio_content PRIVATE bcrypt)
endif()
target_compile_definitions(hs_audio_content PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_audio_content)
endif()

if(HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS OR HS_BUILD_ASSET_PIPELINE_TESTS)
add_executable(hs_generation Source/Apps/AssetGeneration/main.cpp)
target_link_libraries(hs_generation PRIVATE hs_core hs_asset_manifest nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(hs_generation PRIVATE bcrypt)
endif()
target_compile_definitions(hs_generation PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_generation)
if(MSVC)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/hs_generation.manifest" CONTENT [=[
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
    </windowsSettings>
  </application>
</assembly>
]=])
    target_sources(hs_generation PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/hs_generation.manifest")
endif()
endif()

if((HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS) AND HS_WITH_BRIXELIZER)
    # Each SDK compiler invocation is single-threaded; bound independent passes.
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS hs_brixelizer_shader_compile=2)
    include(cmake/BrixelizerSdk.cmake)
    hs_add_brixelizer_sdk()
endif()

if(HS_BUILD_RUNTIME)
add_library(
    hs_renderer_d3d12 STATIC
    Source/RendererD3D12/Private/render_graph.cpp
    Source/RendererD3D12/Private/render_graph_validation.cpp
    Source/RendererD3D12/Private/render_features.cpp
    Source/RendererD3D12/Private/renderer_post_subgraphs.cpp
    Source/RendererD3D12/Private/persistent_gpu_scene.cpp
    Source/RendererD3D12/Private/residency_manager.cpp
    Source/RendererD3D12/Private/streaming_manager.cpp
    Source/RendererD3D12/Private/skin_cache.cpp
    Source/RendererD3D12/Private/renderer_diagnostics.cpp
    Source/RendererD3D12/Private/renderer.cpp
    Source/RendererD3D12/Private/renderer_capture.cpp
    Source/RendererD3D12/Private/renderer_device.cpp
    Source/RendererD3D12/Private/vfx_persistent_state.cpp
    Source/RendererD3D12/Private/vfx_ribbon_state.cpp
    Source/RendererD3D12/Private/renderer_frame.cpp
    Source/RendererD3D12/Private/renderer_pipeline.cpp
    Source/RendererD3D12/Private/renderer_pipeline_cache.cpp
    Source/RendererD3D12/Private/renderer_resources.cpp
    Source/RendererD3D12/Private/renderer_volume_texture.cpp
    Source/RendererD3D12/Private/renderer_resident_assets.cpp
    Source/RendererD3D12/Private/renderer_resident_generation.cpp
    Source/RendererD3D12/Private/renderer_environment.cpp
    Source/RendererD3D12/Private/renderer_bsdf.cpp
    Source/RendererD3D12/Private/renderer_material_sidecar.cpp
    Source/RendererD3D12/Private/renderer_view_resources.cpp
    Source/RendererD3D12/Private/view_state.cpp
    Source/RendererD3D12/Private/reconstruction_plan.cpp
    Source/RendererD3D12/Private/renderer_reconstruction.cpp
    Source/RendererD3D12/Private/renderer_reconstruction_provider.cpp
    Source/RendererD3D12/Private/renderer_reconstruction_guides.cpp
    Source/RendererD3D12/Private/renderer_xess.cpp
    Source/RendererD3D12/Private/renderer_display.cpp
    Source/RendererD3D12/Private/renderer_display_options.cpp
    Source/RendererD3D12/Private/renderer_display_outputs.cpp
    Source/RendererD3D12/Private/renderer_cluster_catalog.cpp
    Source/RendererD3D12/Private/renderer_cluster_pipeline.cpp
    Source/RendererD3D12/Private/renderer_cluster_resources.cpp
    Source/RendererD3D12/Private/renderer_cluster_streaming.cpp
    Source/RendererD3D12/Private/renderer_asset_io.cpp
    Source/RendererD3D12/Private/renderer_texture_streaming.cpp
    Source/RendererD3D12/Private/renderer_texture_feedback.cpp
    Source/RendererD3D12/Private/renderer_cluster_graph.cpp
    Source/RendererD3D12/Private/renderer_ambient_occlusion.cpp
    Source/RendererD3D12/Private/renderer_adaptive_shading.cpp
    Source/RendererD3D12/Private/renderer_screen_overlay.cpp
    Source/RendererD3D12/Private/renderer_lighting.cpp
    Source/RendererD3D12/Private/renderer_subsurface.cpp
    Source/RendererD3D12/Private/renderer_reflections.cpp
    Source/RendererD3D12/Private/renderer_transparency.cpp
    Source/RendererD3D12/Private/renderer_hero_work.cpp
    Source/RendererD3D12/Private/renderer_probe.cpp
    Source/RendererD3D12/Private/renderer_ltc.cpp
    Source/RendererD3D12/Private/renderer_hdr_post.cpp
    Source/RendererD3D12/Private/renderer_scene_geometry.cpp
    Source/RendererD3D12/Private/renderer_entity_surface.cpp
    Source/RendererD3D12/Private/renderer_decal_surface.cpp
    Source/RendererD3D12/Private/renderer_decal_materials.cpp
    Source/RendererD3D12/Private/renderer_views.cpp
    Source/RendererD3D12/Private/renderer_settings.cpp
    Source/RendererD3D12/Private/renderer_named_animation.cpp
    Source/RendererD3D12/Private/renderer_brixelizer_gi.cpp
    Source/RendererD3D12/Private/renderer_brixelizer_integration.cpp
    Source/RendererD3D12/Private/renderer_virtual_shadow.cpp
    Source/RendererD3D12/Private/renderer_local_shadow.cpp
    Source/RendererD3D12/Private/renderer_hlod.cpp
    Source/RendererD3D12/Private/renderer_hlod_runtime.cpp
    Source/RendererD3D12/Private/renderer_local_probe.cpp
    Source/RendererD3D12/Private/renderer_local_probe_runtime.cpp
    Source/RendererD3D12/Private/renderer_volumetrics.cpp
    Source/RendererD3D12/Private/renderer_feature_resolve.cpp
    Source/RendererD3D12/Private/renderer_light_list.cpp
    Source/RendererD3D12/Private/renderer_hero.cpp
    Source/RendererD3D12/Private/renderer_environment_mesh.cpp
    Source/RendererD3D12/Private/renderer_geometry_upload.cpp
    Source/RendererD3D12/Private/renderer_geometry.cpp
    Source/RendererD3D12/Private/renderer_procedural_geometry.cpp
    Source/RendererD3D12/Private/renderer_visibility.cpp
    Source/RendererD3D12/Private/renderer_visibility_resources.cpp
    Source/RendererD3D12/Private/renderer_lod.cpp
    Source/RendererD3D12/Private/renderer_ui.cpp
)
if(NOT HS_SINGLE_CONFIG_RELEASE)
    target_sources(
        hs_renderer_d3d12 PRIVATE
        $<$<CONFIG:Debug>:${imgui_SOURCE_DIR}/imgui.cpp>
        $<$<CONFIG:Debug>:${imgui_SOURCE_DIR}/imgui_draw.cpp>
        $<$<CONFIG:Debug>:${imgui_SOURCE_DIR}/imgui_tables.cpp>
        $<$<CONFIG:Debug>:${imgui_SOURCE_DIR}/imgui_widgets.cpp>
        $<$<CONFIG:Debug>:${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp>
        $<$<CONFIG:Debug>:${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp>
    )
    target_include_directories(
        hs_renderer_d3d12 PRIVATE "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends"
    )
endif()
target_include_directories(hs_renderer_d3d12 PUBLIC Source/RendererD3D12/Public)
target_include_directories(
    hs_renderer_d3d12
    BEFORE PRIVATE "${agility_SOURCE_DIR}/build/native/include"
)
target_link_libraries(
    hs_renderer_d3d12
    PUBLIC hs_core hs_render_data hs_render_data_values
    PRIVATE D3D12MemoryAllocator WinPixEventRuntime DirectXTex d3d12 dxgi dxguid windowscodecs gdi32
)
target_compile_definitions(hs_renderer_d3d12 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
include(cmake/RenderSDKs.cmake)
hs_configure_render_sdks()
include(cmake/DirectStorageSdk.cmake)
hs_configure_directstorage_sdk(hs_renderer_d3d12)
if(HS_WITH_BRIXELIZER)
    target_link_libraries(hs_renderer_d3d12 PRIVATE hs_brixelizer_native)
    target_compile_definitions(hs_renderer_d3d12 PRIVATE HS_WITH_BRIXELIZER=1)
    set_property(SOURCE Source/RendererD3D12/Private/renderer_brixelizer_gi.cpp
        APPEND PROPERTY INCLUDE_DIRECTORIES "${HS_BRIXELIZER_PRIVATE_INCLUDE}")
endif()
target_compile_definitions(
    hs_renderer_d3d12 PRIVATE $<$<CONFIG:Debug>:HS_DEVELOPMENT_TOOLS=1>
)
if(HS_ENABLE_VALIDATION)
    target_compile_definitions(hs_renderer_d3d12 PRIVATE USE_PIX=1)
else()
    target_compile_definitions(hs_renderer_d3d12 PRIVATE USE_PIX_RETAIL=1)
endif()
hs_configure_target(hs_renderer_d3d12)
if(MSVC)
    # Keep /Od, assertions and checked iterators; inline small STL/math calls in
    # the CPU frame path instead of stalling GPU submission behind Debug calls.
    set_property(SOURCE
        Source/RendererD3D12/Private/renderer_frame.cpp
        Source/RendererD3D12/Private/render_graph.cpp
        Source/RendererD3D12/Private/render_graph_validation.cpp
        APPEND PROPERTY COMPILE_OPTIONS "$<$<CONFIG:Debug>:/Ob2>")
endif()

add_library(
    hs_runtime STATIC
    Source/Runtime/Private/application.cpp
    Source/Runtime/Private/shipping_content.cpp
    Source/Runtime/Private/audio_engine.cpp
    Source/Runtime/Private/experiment_spec.cpp
    Source/Runtime/Private/playtest_recording.cpp
    Source/Runtime/Private/save_store.cpp
    Source/Runtime/Private/vfx_catalog.cpp
    Source/Runtime/Private/vfx_typed_frame_adapter.cpp
    Source/Runtime/Private/vfx_upgrade_dispatch.cpp
    Source/Runtime/Private/vfx_typed_decal_commands.cpp
    Source/Runtime/Private/vfx_typed_volume_commands.cpp
    Source/Runtime/Private/vfx_typed_distortion_commands.cpp
    Source/Runtime/Private/vfx_typed_light_commands.cpp
    Source/Runtime/Private/vfx_typed_ballistic_commands.cpp
    Source/Runtime/Private/vfx_typed_mote_commands.cpp
    Source/Runtime/Private/vfx_typed_flash_commands.cpp
    Source/Runtime/Private/vfx_typed_ground_commands.cpp
    Source/Runtime/Private/vfx_typed_mesh_commands.cpp
    Source/Runtime/Private/vfx_typed_fresnel_commands.cpp
    Source/Runtime/Private/vfx_typed_ribbon_commands.cpp
    Source/Runtime/Private/vfx_typed_screen_commands.cpp
    Source/Runtime/Private/vfx_typed_entity_surface_commands.cpp
    Source/Runtime/Private/window.cpp
)
target_sources(
    hs_runtime PRIVATE
)
target_link_libraries(
    hs_runtime
    PUBLIC hs_runtime_contracts hs_gameplay hs_render_data
    PRIVATE hs_jobs hs_presentation hs_renderer_d3d12 $<$<NOT:$<CONFIG:Release>>:hs_asset_manifest> nlohmann_json::nlohmann_json
            user32 shell32 xaudio2 ole32
)
target_compile_definitions(hs_runtime PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_definitions(
    hs_runtime PRIVATE $<$<NOT:$<CONFIG:Release>>:HS_DEVELOPMENT_TOOLS=1>
)
hs_configure_target(hs_runtime)
add_library(hs_runtime_audio_io OBJECT EXCLUDE_FROM_ALL
    Source/Runtime/Private/audio_engine_io.cpp)
target_link_libraries(hs_runtime_audio_io PRIVATE hs_runtime_contracts hs_asset_manifest nlohmann_json::nlohmann_json)
target_compile_definitions(hs_runtime_audio_io PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
hs_configure_target(hs_runtime_audio_io)
target_sources(hs_runtime PRIVATE
    $<$<NOT:$<CONFIG:Release>>:$<TARGET_OBJECTS:hs_runtime_audio_io>>)
add_library(hs_runtime_vfx_io OBJECT EXCLUDE_FROM_ALL
    Source/Runtime/Private/vfx_catalog_io.cpp)
target_link_libraries(hs_runtime_vfx_io PRIVATE hs_vfx_image_io)
hs_configure_target(hs_runtime_vfx_io)
target_sources(hs_runtime PRIVATE
    $<$<NOT:$<CONFIG:Release>>:$<TARGET_OBJECTS:hs_runtime_vfx_io>>)
target_link_libraries(hs_runtime PRIVATE $<$<NOT:$<CONFIG:Release>>:hs_vfx_image_io>)
target_link_libraries(hs_runtime PRIVATE $<$<NOT:$<CONFIG:Release>>:hs_presentation_io>)
target_link_libraries(hs_runtime PRIVATE $<$<NOT:$<CONFIG:Release>>:hs_game_rules_io>)
endif()

if(HS_BUILD_CONTENT_TOOLS OR (HS_BUILD_RUNTIME AND (NOT HS_SINGLE_CONFIG_RELEASE OR HS_BUILD_TESTS)))
find_program(
    HS_DXC
    NAMES dxc.exe
    REQUIRED
    HINTS
        "$ENV{WindowsSdkBinPath}/x64"
)
cmake_path(GET HS_DXC PARENT_PATH HS_DXC_TOOL_DIRECTORY)
set(HS_DXC_TOOL_INPUTS "${HS_DXC}")
foreach(dll IN ITEMS dxcompiler.dll dxil.dll)
    if(NOT EXISTS "${HS_DXC_TOOL_DIRECTORY}/${dll}")
        message(FATAL_ERROR "DXC companion ${dll} is missing beside ${HS_DXC}")
    endif()
    list(APPEND HS_DXC_TOOL_INPUTS "${HS_DXC_TOOL_DIRECTORY}/${dll}")
endforeach()
set(HS_SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/project_hs.hlsl")
set(HS_GPU_SCENE_TABLES_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/gpu_scene_tables.hlsli")
set(HS_HZB_REGION_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/hzb_region.hlsli")
set(HS_GEOMETRY_SHADER_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/geometry.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/geometry_records.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/surface_layout.hlsli")
set(HS_VISIBILITY_SHADER_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/visibility_buffer.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/visibility_records.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/triangle_sample.hlsli")
set(HS_MATERIAL_SURFACE_LIBRARY
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/entity_surface.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/texture_streaming.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_surface.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_parameters.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_sidecar.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/bsdf_layers.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/bsdf_metallic_roughness.hlsli")

endif()

if(HS_BUILD_RUNTIME)
function(hs_copy_runtime_asset source destination)
    get_filename_component(destination_directory "${destination}" DIRECTORY)
    add_custom_command(
        OUTPUT "${destination}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${destination_directory}"
        COMMAND ${CMAKE_COMMAND} -E copy "${source}" "${destination}"
        DEPENDS "${source}"
        VERBATIM
    )
    set(HS_RUNTIME_ASSET_OUTPUTS ${HS_RUNTIME_ASSET_OUTPUTS} "${destination}" PARENT_SCOPE)
endfunction()


hs_copy_runtime_asset(
    "${agility_SOURCE_DIR}/build/native/bin/x64/D3D12Core.dll"
    "${CMAKE_CURRENT_BINARY_DIR}/D3D12/D3D12Core.dll")
hs_copy_runtime_asset(
    "${agility_SOURCE_DIR}/build/native/bin/x64/d3d12SDKLayers.dll"
    "${CMAKE_CURRENT_BINARY_DIR}/D3D12/d3d12SDKLayers.dll")
hs_copy_runtime_asset(
    "${winpix_SOURCE_DIR}/bin/x64/WinPixEventRuntime.dll"
    "${CMAKE_CURRENT_BINARY_DIR}/WinPixEventRuntime.dll")
if(NOT HS_SINGLE_CONFIG_RELEASE)
    hs_copy_runtime_asset(
        "${HS_FONT_DIRECTORY}/NotoSansKR.ttf"
        "${CMAKE_CURRENT_BINARY_DIR}/Fonts/NotoSansKR.ttf")
endif()
hs_deploy_render_sdks()
add_custom_target(hs_runtime_native_assets DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/D3D12/D3D12Core.dll"
    "${CMAKE_CURRENT_BINARY_DIR}/D3D12/d3d12SDKLayers.dll"
    "${CMAKE_CURRENT_BINARY_DIR}/WinPixEventRuntime.dll"
    ${HS_RENDER_SDK_NATIVE_OUTPUTS})
add_custom_target(hs_runtime_assets ALL DEPENDS ${HS_RUNTIME_ASSET_OUTPUTS})

add_executable(ProjectHS Source/Apps/ProjectHS/main.cpp
    Source/Apps/ProjectHS/command_line.cpp)
target_link_libraries(ProjectHS PRIVATE hs_runtime)
target_include_directories(ProjectHS BEFORE PRIVATE "${agility_SOURCE_DIR}/build/native/include")
target_compile_definitions(ProjectHS PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(ProjectHS PRIVATE $<$<NOT:$<CONFIG:Release>>:HS_DEVELOPMENT_TOOLS=1>)
if(HS_ENABLE_VALIDATION)
    target_compile_definitions(ProjectHS PRIVATE HS_ENABLE_VALIDATION=1)
endif()
if(HS_ENABLE_GPU_VALIDATION)
    target_compile_definitions(ProjectHS PRIVATE HS_ENABLE_GPU_VALIDATION=1)
endif()
hs_configure_target(ProjectHS)
if(MSVC)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ProjectHS.manifest" CONTENT [=[
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
    </windowsSettings>
  </application>
</assembly>
]=])
    target_sources(ProjectHS PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ProjectHS.manifest")
endif()
add_dependencies(ProjectHS hs_runtime_assets)

add_executable(hs_experiment Source/Apps/Experiment/main.cpp)
target_link_libraries(
    hs_experiment PRIVATE hs_runtime hs_gameplay hs_game_rules_io hs_presentation hs_asset_manifest nlohmann_json::nlohmann_json dbghelp
)
target_compile_definitions(
    hs_experiment PRIVATE HS_ENABLE_VALIDATION=1 WIN32_LEAN_AND_MEAN NOMINMAX
                          $<$<NOT:$<CONFIG:Release>>:HS_DEVELOPMENT_TOOLS=1>
)
hs_configure_target(hs_experiment)
if(MSVC)
    target_sources(hs_experiment PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ProjectHS.manifest")
endif()
add_dependencies(hs_experiment hs_runtime_assets)

add_executable(hs_render_test_runner
    Source/Apps/RenderTestRunner/main.cpp
    Source/Apps/Common/world_transport_scene.cpp)
target_link_libraries(hs_render_test_runner PRIVATE hs_vfx_program_io)
target_link_libraries(
    hs_render_test_runner PRIVATE hs_renderer_d3d12 hs_render_data hs_asset_manifest hs_jobs hs_game_rules_io hs_environment_layout nlohmann_json::nlohmann_json
)
target_compile_definitions(hs_render_test_runner PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_sources(hs_render_test_runner PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ProjectHS.manifest")
endif()
if(HS_BUILD_CONTENT_TOOLS)
    add_executable(hs_scene_probe_bake
        Source/Apps/SceneProbeBake/main.cpp
        Source/Apps/SceneProbeBake/scene_probe_action.cpp
        Source/Apps/Common/world_transport_scene.cpp)
    target_link_libraries(hs_scene_probe_bake PRIVATE hs_asset_pipeline hs_renderer_d3d12 hs_render_data
        hs_asset_manifest hs_jobs hs_game_rules_io hs_environment_layout nlohmann_json::nlohmann_json)
    target_compile_definitions(hs_scene_probe_bake PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    hs_configure_target(hs_scene_probe_bake)
    if(MSVC)
        target_sources(hs_scene_probe_bake PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ProjectHS.manifest")
    endif()
    add_dependencies(hs_scene_probe_bake hs_runtime_assets)
    add_executable(hs_static_shadow_bake
        Source/Apps/StaticShadowBake/main.cpp
        Source/Apps/Common/world_transport_scene.cpp
        Source/Apps/RenderTestRunner/static_shadow_action.cpp)
    target_link_libraries(hs_static_shadow_bake PRIVATE hs_asset_pipeline hs_renderer_d3d12 hs_render_data
        hs_asset_manifest hs_game_rules_io hs_environment_layout nlohmann_json::nlohmann_json)
    target_compile_definitions(hs_static_shadow_bake PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    hs_configure_target(hs_static_shadow_bake)
    if(MSVC)
        target_sources(hs_static_shadow_bake PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ProjectHS.manifest")
    endif()
    add_dependencies(hs_static_shadow_bake hs_runtime_assets)
endif()
hs_configure_target(hs_render_test_runner)
add_dependencies(hs_render_test_runner hs_runtime_assets)
endif()

if(HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS)
    add_executable(hs_combat_sim Source/Apps/CombatSim/main.cpp)
    target_link_libraries(
        hs_combat_sim PRIVATE hs_gameplay hs_game_rules_io hs_asset_manifest nlohmann_json::nlohmann_json
    )
    target_compile_definitions(hs_combat_sim PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    hs_configure_target(hs_combat_sim)
endif()

if(HS_BUILD_CONTENT_TOOLS OR HS_BUILD_RENDER_DATA_TESTS)
add_library(hs_meshoptimizer_cook STATIC
    ThirdParty/meshoptimizer/src/indexgenerator.cpp
    ThirdParty/meshoptimizer/src/clusterizer.cpp
    ThirdParty/meshoptimizer/src/meshletutils.cpp
    ThirdParty/meshoptimizer/src/partition.cpp
    ThirdParty/meshoptimizer/src/simplifier.cpp)
target_include_directories(hs_meshoptimizer_cook PUBLIC ThirdParty/meshoptimizer/src)
add_library(hs_geometry_cooker STATIC
    Source/Apps/Content/geometry_cooker.cpp
    Source/Apps/Content/geometry_hierarchy_cooker.cpp
    Source/Apps/Content/geometry_error.cpp)
target_include_directories(hs_geometry_cooker PUBLIC Source/Apps/Content)
target_link_libraries(hs_geometry_cooker PUBLIC hs_render_data PRIVATE hs_meshoptimizer_cook)
hs_configure_target(hs_geometry_cooker)
if(MSVC)
    set_source_files_properties(Source/Apps/Content/geometry_error.cpp PROPERTIES
        COMPILE_OPTIONS /fp:strict
        COMPILE_DEFINITIONS HS_GEOMETRY_ERROR_FP_STRICT=1)
    set_source_files_properties(Source/Apps/Content/character_products.cpp PROPERTIES
        COMPILE_OPTIONS /fp:strict)
endif()
add_library(hs_scene_hlod_cooker STATIC Source/Apps/Content/scene_hlod_cooker.cpp)
target_link_libraries(hs_scene_hlod_cooker PUBLIC hs_geometry_cooker PRIVATE hs_meshoptimizer_cook hs_jobs)
hs_configure_target(hs_scene_hlod_cooker)
# Asset cooking can perform hundreds of thousands of BVH distance queries per
# HLOD group. Keep the runtime, renderer and tests in the regular checked Debug
# configuration, but optimize only these offline geometry-building libraries.
# CMP0184 (NEW under the project's CMake 4.4 minimum) allows runtime checks to
# be disabled per target; MSVC otherwise rejects /O2 together with /RTC1.
if(MSVC)
    foreach(hs_offline_geometry_target IN ITEMS
        hs_meshoptimizer_cook hs_geometry_cooker hs_scene_hlod_cooker)
        set_property(TARGET ${hs_offline_geometry_target} PROPERTY MSVC_RUNTIME_CHECKS "")
        # /O2 does not override the explicit /Ob0 inherited from Debug.
        target_compile_options(${hs_offline_geometry_target} PRIVATE "$<$<CONFIG:Debug>:/O2;/Ob2>")
    endforeach()
endif()
endif()
if(HS_BUILD_CONTENT_TOOLS)
configure_file(cmake/hs_build_config.json.in
    "${CMAKE_CURRENT_BINARY_DIR}/hs_build_config.json" @ONLY)
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

add_library(hs_vfx_authoring STATIC
    Source/Apps/Content/vfx_spec_validation.cpp
    Source/Apps/Content/vfx_texture_validation.cpp
    Source/Apps/Content/vfx_gradient_cooker.cpp
    Source/Apps/Content/vfx_program_cooker.cpp
    Source/Apps/Content/vfx_runtime_cooker.cpp
)
target_link_libraries(hs_vfx_authoring PUBLIC hs_render_data_values
    PRIVATE hs_render_data nlohmann_json::nlohmann_json DirectXTex)
target_include_directories(hs_vfx_authoring PUBLIC Source/RendererD3D12/Public)
hs_configure_target(hs_vfx_authoring)
add_executable(hs_vfx_prepare Source/Apps/Content/vfx_prepare_main.cpp)
target_link_libraries(hs_vfx_prepare PRIVATE hs_vfx_authoring nlohmann_json::nlohmann_json)
target_compile_definitions(hs_vfx_prepare PRIVATE HS_VFX_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
hs_configure_target(hs_vfx_prepare)

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
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Characters/Archer/DiveForward.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Characters/Archer/ReactBack.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Characters/Archer/ReactFront.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Characters/Archer/ReactRight.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Characters/Archer/RunForwardStop.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Animations/Characters/Archer/WalkForward.fbx"
)
file(GLOB_RECURSE HS_ENVIRONMENT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/Environment/*.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/Environment/*.json"
)
file(GLOB HS_VFX_TEXTURE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/VFX/Meshes/*.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/VFX/*.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/VFX/*.dds"
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
include(cmake/AssetActions.cmake)
hs_add_asset_action(
    NAME hs_audio_products
    BUILDER hs_audio_content
    RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/audio/result.json"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/audio/result.d"
    STATIC_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/GameData/audio_cues.json"
    ARGS --produce --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
         --build-root "${CMAKE_CURRENT_BINARY_DIR}"
)
add_executable(hs_game_data
    Source/Apps/GameData/main.cpp
    Source/Apps/Content/game_data_cooker.cpp
    Source/Apps/Content/release_presentation_codegen.cpp
    Source/Apps/Content/release_simulation_codegen.cpp
    Source/Apps/Content/game_data_validation.cpp
    Source/Apps/Content/content_source_inventory.cpp
    Source/Apps/Content/content_paths.cpp
)
target_link_libraries(hs_game_data PRIVATE hs_asset_pipeline hs_presentation_io)
target_include_directories(hs_game_data PRIVATE Source/Apps/Content)
target_link_libraries(hs_game_data PRIVATE
    hs_core hs_game_rules hs_presentation nlohmann_json::nlohmann_json bcrypt)
target_compile_definitions(hs_game_data PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_game_data)
set(HS_GAMEPLAY_IDENTITY_INPUTS ${HS_ARCHER_ASSET_SOURCES})
foreach(category IN ITEMS stats level characters skills upgrades enemies relics spawn_schedule bosses materials ui_strings)
    list(APPEND HS_GAMEPLAY_IDENTITY_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/GameData/${category}.json")
endforeach()
set(HS_FULL_IDENTITY_INPUTS
    ${HS_GAME_DATA_SOURCES} ${HS_ARCHER_ASSET_SOURCES} ${HS_MONSTER_ASSET_SOURCES}
    ${HS_MONSTER_TEXTURE_SOURCES} ${HS_ENVIRONMENT_SOURCES} ${HS_AUDIO_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Environment/environment_meshes.json")
foreach(mode IN ITEMS simulation-payload presentation-payload gameplay-identity content-identity finalize-simulation finalize-presentation)
    set(action_result "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/${mode}/result.json")
    if(mode STREQUAL "simulation-payload")
        set(action_inputs)
        foreach(category IN ITEMS stats level characters skills upgrades enemies relics spawn_schedule bosses)
            list(APPEND action_inputs
                "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/GameData/${category}.json")
        endforeach()
        list(APPEND action_inputs
            "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/game.schema.json")
        # Spawn bounds currently use camera data from characters.json.
    elseif(mode STREQUAL "presentation-payload")
        set(action_inputs
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/GameData/characters.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/GameData/relics.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/game.schema.json")
    elseif(mode STREQUAL "gameplay-identity")
        set(action_inputs ${HS_GAMEPLAY_IDENTITY_INPUTS})
    elseif(mode STREQUAL "content-identity")
        set(action_inputs ${HS_FULL_IDENTITY_INPUTS})
    elseif(mode STREQUAL "finalize-simulation")
        set(action_inputs)
        set(action_upstream
            "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/simulation-payload/result.json"
            "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/gameplay-identity/result.json")
    else()
        set(action_inputs)
        set(action_upstream
            "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/presentation-payload/result.json"
            "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/content-identity/result.json")
    endif()
    hs_add_asset_action(
        NAME "hs_game_data_${mode}"
        BUILDER hs_game_data
        RESULT "${action_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/${mode}/result.d"
        STATIC_INPUTS ${action_inputs}
        UPSTREAM_RESULTS ${action_upstream}
        ARGS "--${mode}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}"
    )
    unset(action_upstream)
endforeach()
add_custom_target(hs_game_data_products
    DEPENDS
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/finalize-simulation/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/finalize-presentation/result.json")
add_executable(hs_content_linker Source/Apps/ContentLinker/main.cpp
    Source/Apps/Content/release_vfx_codegen.cpp
    Source/Apps/Content/character_products.cpp)
target_compile_definitions(hs_content_linker PRIVATE HS_SHIPPING_PACK_RELATIVE_PATH="${HS_SHIPPING_PACK_RELATIVE_PATH}")
target_link_libraries(hs_content_linker PRIVATE hs_asset_pipeline hs_vfx_image_io hs_geometry_cooker
    hs_game_rules hs_presentation_io nlohmann_json::nlohmann_json)
hs_configure_target(hs_content_linker)
add_executable(hs_environment
    Source/Apps/Environment/main.cpp
    Source/Apps/Content/environment_histogram_cooker.cpp
    Source/Apps/Content/environment_texture_cooker.cpp
    Source/Apps/Content/environment_mesh_cooker.cpp)
target_link_libraries(hs_environment PRIVATE hs_asset_pipeline)
target_include_directories(hs_environment PRIVATE Source/Apps/Content)
target_link_libraries(hs_environment PRIVATE
    hs_core nlohmann_json::nlohmann_json DirectXTex d3d11 dxgi ole32 bcrypt)
target_compile_definitions(hs_environment PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_environment)
set(HS_ENVIRONMENT_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/Environment/project_hs_environment_materials_v9_LEAN.json")
foreach(mode IN ITEMS meshes)
    set(action_inputs
        "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Environment/environment_meshes.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Environment/meadow_surface_meshes.json")
    hs_add_asset_action(
        NAME "hs_environment_${mode}"
        BUILDER hs_environment
        RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/${mode}/result.json"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/${mode}/result.d"
        STATIC_INPUTS ${action_inputs}
        ARGS "--${mode}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()
set_property(GLOBAL APPEND PROPERTY JOB_POOLS hs_environment_texture=1)
set(HS_ENVIRONMENT_TEXTURE_BYPRODUCTS
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/histogram/result.json")
set(HS_ENVIRONMENT_TEXTURE_RESULTS)
foreach(group IN ITEMS terrain rock bark grass leaf ground_detail)
    if(group STREQUAL "terrain")
        set(roles base_color surface_ra normal detail_normal height)
    elseif(group STREQUAL "ground_detail")
        set(roles base_color surface_ra normal)
    elseif(group STREQUAL "rock" OR group STREQUAL "bark")
        set(roles base_color surface_ra normal detail_normal)
    else()
        set(roles base_color normal roughness)
    endif()
    foreach(role IN LISTS roles)
        set(action_result "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/textures/${group}/${role}/result.json")
        list(APPEND HS_ENVIRONMENT_TEXTURE_BYPRODUCTS "${action_result}")
        list(APPEND HS_ENVIRONMENT_TEXTURE_RESULTS "${action_result}")
    endforeach()
endforeach()
set(HS_ENVIRONMENT_TEXTURE_FAMILY_STAMP
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/textures/family.stamp")
set(HS_ENVIRONMENT_TEXTURE_FAMILY_DEPFILE
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/textures/family.d")
add_custom_command(
    OUTPUT "${HS_ENVIRONMENT_TEXTURE_FAMILY_STAMP}"
    COMMAND "$<TARGET_FILE:hs_environment>" --texture-batch
        --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --build-root "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS hs_environment "${HS_ENVIRONMENT_MANIFEST}"
        "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/Environment/baked_materials.json"
    DEPFILE "${HS_ENVIRONMENT_TEXTURE_FAMILY_DEPFILE}"
    BYPRODUCTS ${HS_ENVIRONMENT_TEXTURE_BYPRODUCTS}
    JOB_POOL hs_environment_texture
    VERBATIM)
foreach(group IN ITEMS terrain rock bark grass leaf ground_detail)
    if(group STREQUAL "terrain")
        set(roles base_color surface_ra normal detail_normal height)
    elseif(group STREQUAL "ground_detail")
        set(roles base_color surface_ra normal)
    elseif(group STREQUAL "rock" OR group STREQUAL "bark")
        set(roles base_color surface_ra normal detail_normal)
    else()
        set(roles base_color normal roughness)
    endif()
    foreach(role IN LISTS roles)
        add_custom_target("hs_environment_texture_${group}_${role}"
            DEPENDS "${HS_ENVIRONMENT_TEXTURE_FAMILY_STAMP}")
    endforeach()
endforeach()
add_custom_target(hs_environment_histogram
    DEPENDS "${HS_ENVIRONMENT_TEXTURE_FAMILY_STAMP}")
add_custom_target(hs_environment_products
    DEPENDS
        "${HS_ENVIRONMENT_TEXTURE_FAMILY_STAMP}"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/meshes/result.json")
add_executable(hs_lighting_probe
    Source/Apps/LightingProbe/main.cpp)
target_link_libraries(hs_lighting_probe PRIVATE hs_asset_pipeline)
target_link_libraries(hs_lighting_probe PRIVATE
    hs_render_data hs_core nlohmann_json::nlohmann_json bcrypt DirectXTex)
target_compile_definitions(hs_lighting_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_lighting_probe)
set(HS_LIGHTING_PROBE_RESULT
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/default_probe/result.json")
hs_add_asset_action(
    NAME hs_lighting_probe_products
    BUILDER hs_lighting_probe
    RESULT "${HS_LIGHTING_PROBE_RESULT}"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/default_probe/result.d"
    STATIC_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Lighting/default_probe.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/lighting_probe.schema.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/Apps/LightingProbe/radiance_hdr.hpp"
    ARGS --produce --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --build-root "${CMAKE_CURRENT_BINARY_DIR}")
add_executable(hs_ltc_lut
    Source/Apps/LtcLut/main.cpp)
target_link_libraries(hs_ltc_lut PRIVATE hs_asset_pipeline)
target_link_libraries(hs_ltc_lut PRIVATE
    hs_render_data hs_core nlohmann_json::nlohmann_json bcrypt)
target_compile_definitions(hs_ltc_lut PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_ltc_lut)
set(HS_LTC_LUT_RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/default_ltc/result.json")
hs_add_asset_action(
    NAME hs_ltc_lut_products
    BUILDER hs_ltc_lut
    RESULT "${HS_LTC_LUT_RESULT}"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/default_ltc/result.d"
    STATIC_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Lighting/default_ltc.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/ltc_lut.schema.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/bsdf_metallic_roughness.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/bsdf_layers.hlsli"
    ARGS --produce --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --build-root "${CMAKE_CURRENT_BINARY_DIR}")
add_executable(hs_render_texture_package
    Source/Apps/RenderTexturePackage/main.cpp)
target_link_libraries(hs_render_texture_package PRIVATE hs_asset_pipeline)
target_link_libraries(hs_render_texture_package PRIVATE
    hs_render_data hs_core nlohmann_json::nlohmann_json bcrypt)
target_compile_definitions(hs_render_texture_package PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_render_texture_package)
set(HS_RENDER_TEXTURE_RESULTS)
set(HS_RENDER_TEXTURE_NAMES
    terrain_gaussian terrain_surface terrain_normal terrain_detail terrain_height terrain_histogram
    rock_basecolor rock_surface rock_normal rock_detail
    bark_basecolor bark_surface bark_normal bark_detail
    grass_basecolor grass_normal grass_roughness
    leaf_basecolor leaf_normal leaf_roughness
    ground_detail_basecolor ground_detail_surface ground_detail_normal)
foreach(texture_name IN LISTS HS_RENDER_TEXTURE_NAMES)
    if(texture_name STREQUAL "terrain_gaussian" OR texture_name STREQUAL "terrain_histogram")
        set(texture_upstream
            "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/histogram/result.json")
    else()
        string(REGEX MATCH "^(.+)_([^_]+)$" ignored "${texture_name}")
        set(texture_group "${CMAKE_MATCH_1}")
        set(texture_role "${CMAKE_MATCH_2}")
        if(texture_role STREQUAL "basecolor")
            set(texture_role base_color)
        elseif(texture_role STREQUAL "surface")
            set(texture_role surface_ra)
        elseif(texture_role STREQUAL "detail")
            set(texture_role detail_normal)
        endif()
        set(texture_upstream
            "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/textures/${texture_group}/${texture_role}/result.json")
    endif()
    set(texture_result
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/environment_${texture_name}/result.json")
    hs_add_asset_action(
        NAME "hs_render_texture_environment_${texture_name}"
        BUILDER hs_render_texture_package
        RESULT "${texture_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/environment_${texture_name}/result.d"
        UPSTREAM_RESULTS "${texture_upstream}"
        ARGS --build-root "${CMAKE_CURRENT_BINARY_DIR}" --name "${texture_name}")
    list(APPEND HS_RENDER_TEXTURE_RESULTS "${texture_result}")
endforeach()
add_executable(hs_render_package
    Source/Apps/RenderPackage/main.cpp
    Source/Apps/RenderMaterialShader/material_graph_parser.cpp)
target_link_libraries(hs_render_package PRIVATE hs_asset_pipeline)
target_link_libraries(hs_render_package PRIVATE
    hs_geometry_cooker hs_material_compiler hs_render_data hs_core nlohmann_json::nlohmann_json bcrypt)
target_compile_definitions(hs_render_package PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_render_package)
add_executable(hs_render_scene_package
    Source/Apps/RenderScenePackage/main.cpp)
target_link_libraries(hs_render_scene_package PRIVATE hs_asset_pipeline)
target_link_libraries(hs_render_scene_package PRIVATE
    hs_scene_hlod_cooker hs_render_data hs_game_rules hs_environment_layout
    nlohmann_json::nlohmann_json bcrypt)
target_compile_definitions(hs_render_scene_package PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_render_scene_package)
add_executable(hs_render_scene_probe
    Source/Apps/RenderSceneProbe/main.cpp)
target_link_libraries(hs_render_scene_probe PRIVATE hs_asset_pipeline)
target_link_libraries(hs_render_scene_probe PRIVATE
    hs_render_data hs_core nlohmann_json::nlohmann_json bcrypt)
target_compile_definitions(hs_render_scene_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_render_scene_probe)
add_executable(hs_render_quality_profiles
    Source/Apps/RenderQualityProfiles/main.cpp)
target_link_libraries(hs_render_quality_profiles PRIVATE hs_asset_pipeline)
target_link_libraries(hs_render_quality_profiles PRIVATE
    hs_render_data hs_core nlohmann_json::nlohmann_json bcrypt)
target_compile_definitions(hs_render_quality_profiles PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_render_quality_profiles)
set(HS_QUALITY_PROFILES_RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/config/quality-profiles/result.json")
hs_add_asset_action(NAME hs_render_quality_profiles_products BUILDER hs_render_quality_profiles
    RESULT "${HS_QUALITY_PROFILES_RESULT}"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/config/quality-profiles/result.d"
    STATIC_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Graphics/quality_profiles.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/quality_profiles.schema.json"
    ARGS --source-root "${CMAKE_CURRENT_SOURCE_DIR}" --build-root "${CMAKE_CURRENT_BINARY_DIR}")
add_executable(hs_render_volumetric_package
    Source/Apps/RenderVolumetricPackage/main.cpp)
target_link_libraries(hs_render_volumetric_package PRIVATE hs_asset_pipeline)
target_link_libraries(hs_render_volumetric_package PRIVATE
    hs_render_data hs_core nlohmann_json::nlohmann_json bcrypt)
target_compile_definitions(hs_render_volumetric_package PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_render_volumetric_package)
set(HS_VOLUMETRIC_MEDIA_RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/volumetric-media/result.json")
hs_add_asset_action(NAME hs_render_volumetric_media BUILDER hs_render_volumetric_package
    RESULT "${HS_VOLUMETRIC_MEDIA_RESULT}"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/volumetric-media/result.d"
    STATIC_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Lighting/volumetrics.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/volumetric_medium.schema.json"
    ARGS --source-root "${CMAKE_CURRENT_SOURCE_DIR}" --build-root "${CMAKE_CURRENT_BINARY_DIR}")
add_executable(hs_render_material_shader
    Source/Apps/RenderMaterialShader/main.cpp
    Source/Apps/RenderMaterialShader/coated_dfg_cooker.cpp
    Source/Apps/RenderMaterialShader/material_graph_parser.cpp)
target_link_libraries(hs_render_material_shader PRIVATE hs_asset_pipeline)
target_link_libraries(hs_render_material_shader PRIVATE
    hs_material_compiler hs_render_data hs_core nlohmann_json::nlohmann_json bcrypt)
target_compile_definitions(hs_render_material_shader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_render_material_shader)
set(HS_RENDER_SHADER_RESULTS)
add_executable(hs_render_runtime_shader
    Source/Apps/RenderRuntimeShader/main.cpp)
target_link_libraries(hs_render_runtime_shader PRIVATE hs_asset_pipeline)
target_link_libraries(hs_render_runtime_shader PRIVATE
    hs_render_data hs_core nlohmann_json::nlohmann_json bcrypt)
target_compile_definitions(hs_render_runtime_shader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_render_runtime_shader)
foreach(family IN ITEMS project geometry visibility static-visibility depth-pyramid skin-cache exposure cluster-refinement ambient-occlusion reconstruction-guides sdk-presentation hdr-post light-list scene-geometry virtual-shadow volumetrics scene-hlod local-probe-bake)
    if(family STREQUAL "project")
        set(family_source project_hs.hlsl)
    elseif(family STREQUAL "visibility")
        set(family_source visibility_buffer.hlsl)
    else()
        string(REPLACE "-" "_" family_source "${family}")
        string(APPEND family_source ".hlsl")
    endif()
    set(family_hzb_inputs)
    if(family STREQUAL "static-visibility" OR family STREQUAL "cluster-refinement")
        list(APPEND family_hzb_inputs "${HS_HZB_REGION_SOURCE}")
    endif()
    set(family_result "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/runtime/${family}/result.json")
    hs_add_asset_action(
        NAME "hs_render_runtime_shader_${family}"
        BUILDER hs_render_runtime_shader
        RESULT "${family_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/runtime/${family}/result.d"
        STATIC_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/${family_source}" ${family_hzb_inputs}
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/entity_surface.hlsli"
            ${HS_DXC_TOOL_INPUTS}
        ARGS "--family=${family}" "--source=${CMAKE_CURRENT_SOURCE_DIR}"
            "--build=${CMAKE_CURRENT_BINARY_DIR}" "--dxc=${HS_DXC}")
    list(APPEND HS_RENDER_SHADER_RESULTS "${family_result}")
endforeach()
if(HS_WITH_BRIXELIZER)
    add_executable(hs_render_brixelizer_shader
        Source/Apps/RenderBrixelizerShaderPackage/main.cpp
        ContentSource/Textures/BrixelizerNoise/samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_8spp.cpp
        Source/Apps/AssetPipeline/action.cpp)
target_link_libraries(hs_render_brixelizer_shader PRIVATE hs_asset_pipeline)
    target_link_libraries(hs_render_brixelizer_shader PRIVATE
        hs_core hs_render_data hs_brixelizer_shader_tables nlohmann_json::nlohmann_json bcrypt)
    target_compile_definitions(hs_render_brixelizer_shader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    hs_configure_target(hs_render_brixelizer_shader)
    set(HS_BRIXELIZER_SHADER_RESULT
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/brixelizer/result.json")
    get_filename_component(HS_BRIXELIZER_CACHE_ROOT "${HS_BRIXELIZER_SDK_ROOT}" DIRECTORY)
    hs_render_sdk_pin(brixelizer HS_BRIXELIZER_PIN)
    hs_add_asset_action(NAME hs_render_brixelizer_shader_action
        BUILDER hs_render_brixelizer_shader
        RESULT "${HS_BRIXELIZER_SHADER_RESULT}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/brixelizer/result.d"
        STATIC_INPUTS "${HS_BRIXELIZER_PIN}"
            "${HS_BRIXELIZER_CACHE_ROOT}/source-files.sha256"
            "${HS_BRIXELIZER_CACHE_ROOT}/sdk-source.zip"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tools/bootstrap-brixelizer-sdk.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BrixelizerSdk.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BrixelizerNative/CMakeLists.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BrixelizerNative/NormalizeShaderDepfile.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/BrixelizerNoise/samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_8spp.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/BrixelizerNoise/FidelityFX-LICENSE.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/BrixelizerNoise/README.md"
        ARGS "--source=${CMAKE_CURRENT_SOURCE_DIR}" "--build=${CMAKE_CURRENT_BINARY_DIR}"
            "--sdk=${HS_BRIXELIZER_SDK_ROOT}" "--pin=${HS_BRIXELIZER_PIN}")
    list(APPEND HS_RENDER_SHADER_RESULTS "${HS_BRIXELIZER_SHADER_RESULT}")
endif()
set(HS_BRIXELIZER_TRANSPORT_ARGS)
set(HS_BRIXELIZER_TRANSPORT_INPUTS)
if(HS_WITH_BRIXELIZER)
    list(APPEND HS_BRIXELIZER_TRANSPORT_ARGS --with-brixelizer)
    list(APPEND HS_BRIXELIZER_TRANSPORT_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/brixelizer_common.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/brixelizer_guides.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/brixelizer_gi_resolve.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/brixelizer_gi_compose.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/brixelizer_cache_export.hlsl")
endif()
set(HS_BSDF_SHADER_RESULT
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/bsdf-preintegrate/result.json")
set(HS_AUTHORED_MATERIAL_NAMES
    trunk_0 trunk_1 trunk_2 leaf_0 leaf_1 leaf_2 rock_0 rock_1 rock_2 rock_3
    grass_0 grass_1 grass_2 grass_3 terrain ground_detail)
set(HS_GROUND_DETAIL_MESH_NAMES
    flower_daisy flower_buttercup flower_violet clover_patch ground_leaves twig_cluster fence_short
    shrub_low stump_roots crate_intact crate_broken barrel_intact barrel_broken cart_broken fence_broken fence_splitrail sack_tied fallen_log rock_mossy)
set(HS_HERO_MATERIAL_NAMES
    archer_skin archer_cloth archer_cutout archer_equipment slime_features slime_body monster_pbr)
set(HS_DEFERRED_MATERIAL_INPUTS)
foreach(name IN LISTS HS_AUTHORED_MATERIAL_NAMES HS_HERO_MATERIAL_NAMES)
    list(APPEND HS_DEFERRED_MATERIAL_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/${name}.json")
endforeach()
hs_add_asset_action(
    NAME hs_render_bsdf_preintegrate
    BUILDER hs_render_material_shader
    RESULT "${HS_BSDF_SHADER_RESULT}"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/bsdf-preintegrate/result.d"
    STATIC_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/bsdf_preintegrate.hlsl"
        "${HS_SHADER_SOURCE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/environment.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/environment_wind.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/grass_deformation.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/particle_collision.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_terrain.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/vfx_ribbon.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/deferred_surface.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/subsurface_diffusion.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/screen_reflections.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/vfx_screen_overlay.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/adaptive_shading.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/adaptive_gtao.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/ambient_occlusion.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/probe_ibl.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/light_list.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/ltc_area.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/scene_geometry.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/scene_geometry.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/local_shadow.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/shadow_data.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/temporal_reconstruction.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/display_transform.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/material_graph.schema.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/Apps/RenderMaterialShader/coated_dfg_cooker.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/Apps/RenderMaterialShader/coated_dfg_cooker.hpp"
        ${HS_DEFERRED_MATERIAL_INPUTS}
        "${HS_GPU_SCENE_TABLES_SOURCE}" ${HS_GEOMETRY_SHADER_SOURCE}
        ${HS_MATERIAL_SURFACE_LIBRARY} ${HS_DXC_TOOL_INPUTS}
        ${HS_BRIXELIZER_TRANSPORT_INPUTS}
    ARGS --bsdf-preintegrate --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --build-root "${CMAKE_CURRENT_BINARY_DIR}" --dxc "${HS_DXC}"
        ${HS_BRIXELIZER_TRANSPORT_ARGS})
list(APPEND HS_RENDER_SHADER_RESULTS "${HS_BSDF_SHADER_RESULT}")
# Intrusive developer PS variants are not part of Release/shipping products.
# Current content generations use the repository's single-config Ninja trees.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    set(HS_HERO_WORK_SHADER_RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/hero-work/result.json")
hs_add_asset_action(
    NAME hs_render_hero_work
    BUILDER hs_render_material_shader
    RESULT "${HS_HERO_WORK_SHADER_RESULT}"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/hero-work/result.d"
    STATIC_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/bsdf_preintegrate.hlsl"
        "${HS_SHADER_SOURCE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/environment.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/environment_wind.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/grass_deformation.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/particle_collision.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_terrain.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/vfx_ribbon.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/deferred_surface.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/subsurface_diffusion.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/screen_reflections.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/vfx_screen_overlay.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/adaptive_shading.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/adaptive_gtao.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/ambient_occlusion.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/probe_ibl.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/light_list.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/ltc_area.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/scene_geometry.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/scene_geometry.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/local_shadow.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/shadow_data.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/temporal_reconstruction.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/display_transform.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/material_graph.schema.json"
        ${HS_DEFERRED_MATERIAL_INPUTS}
        "${HS_GPU_SCENE_TABLES_SOURCE}" ${HS_GEOMETRY_SHADER_SOURCE}
        ${HS_MATERIAL_SURFACE_LIBRARY} ${HS_DXC_TOOL_INPUTS}
    ARGS --hero-work --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --build-root "${CMAKE_CURRENT_BINARY_DIR}" --dxc "${HS_DXC}")
    list(APPEND HS_RENDER_SHADER_RESULTS "${HS_HERO_WORK_SHADER_RESULT}")
endif()
add_custom_target(hs_shaders DEPENDS ${HS_RENDER_SHADER_RESULTS})

set(HS_RENDER_ACTION_RESULTS)
foreach(name IN LISTS HS_AUTHORED_MATERIAL_NAMES)
    set(action_result "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/${name}/result.json")
    set(material_schema "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/material_graph.schema.json")
    set(material_upstream "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/${name}/result.json")
    set(parameter_result "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/${name}/parameters/result.json")
    set(material_source "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/${name}.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${material_source}")
    file(READ "${material_source}" material_document)
    string(JSON material_version GET "${material_document}" version)
    set(material_texture_results)
    if(material_version GREATER_EQUAL 3)
        string(JSON texture_count LENGTH "${material_document}" textures)
        if(texture_count LESS 3 OR texture_count GREATER 6)
            message(FATAL_ERROR "${name}: material requires three to six texture declarations")
        endif()
        math(EXPR last_texture "${texture_count} - 1")
        foreach(texture RANGE 0 ${last_texture})
            string(JSON product GET "${material_document}" textures ${texture} product_id)
            if(NOT product MATCHES "^render/texture/(environment_[A-Za-z0-9_]+)\\.rpkg$")
                message(FATAL_ERROR "${name}: unsupported material texture product '${product}'")
            endif()
            list(APPEND material_texture_results
                "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/${CMAKE_MATCH_1}/result.json")
        endforeach()
    else()
        foreach(role IN ITEMS base_color surface normal detail)
            string(JSON product GET "${material_document}" textures ${role})
            if(product)
                cmake_path(GET product STEM texture_name)
                list(APPEND material_texture_results
                    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/${texture_name}/result.json")
            endif()
        endforeach()
    endif()
    list(REMOVE_DUPLICATES material_texture_results)
    set(material_library_inputs)
    if(name STREQUAL "terrain")
        list(APPEND material_library_inputs "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_terrain.hlsli")
    endif()
    set(material_geometry_results)
    if(NOT name STREQUAL "terrain" AND NOT name STREQUAL "ground_detail")
        list(APPEND material_geometry_results "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/meshes/result.json")
    endif()
    hs_add_asset_action(
        NAME "hs_render_material_shader_${name}"
        BUILDER hs_render_material_shader
        RESULT "${material_upstream}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/${name}/result.d"
        STATIC_INPUTS
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/${name}.json"
            ${HS_DEFERRED_MATERIAL_INPUTS}
            "${material_schema}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/project_hs.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/environment.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/environment_wind.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/grass_deformation.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/particle_collision.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/local_probe_capture.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/local_probe_draw.hlsli"
            ${material_library_inputs}
            ${HS_MATERIAL_SURFACE_LIBRARY}
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/vfx_ribbon.hlsli"
            "${HS_GPU_SCENE_TABLES_SOURCE}"
            "${HS_GEOMETRY_SHADER_SOURCE}"
            ${HS_VISIBILITY_SHADER_SOURCE}
            ${HS_DXC_TOOL_INPUTS}
        ARGS --name "${name}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}" --dxc "${HS_DXC}")
    hs_add_asset_action(
        NAME "hs_render_material_parameters_${name}"
        BUILDER hs_render_material_shader
        RESULT "${parameter_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/${name}/parameters/result.d"
        STATIC_INPUTS "${material_source}" "${material_schema}"
        ARGS --parameters --name "${name}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}")
    list(APPEND HS_RENDER_SHADER_RESULTS "${material_upstream}")
    list(APPEND HS_RENDER_ACTION_RESULTS "${action_result}" "${parameter_result}")
    hs_add_asset_action(
        NAME "hs_render_${name}"
        BUILDER hs_render_package
        RESULT "${action_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/${name}/result.d"
        STATIC_INPUTS
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/${name}.json"
            "${material_schema}"
        UPSTREAM_RESULTS
            ${material_geometry_results}
            ${material_texture_results}
            ${material_upstream}
            ${parameter_result}
        ARGS --name "${name}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()
set(HS_GROUND_DETAIL_GEOMETRY_RESULTS)
set(HS_HLOD_GROUND_DETAIL_GEOMETRY_RESULTS)
list(APPEND HS_GROUND_DETAIL_MESH_NAMES meadow_ground meadow_water)
foreach(name IN LISTS HS_GROUND_DETAIL_MESH_NAMES)
    if(name STREQUAL "meadow_ground")
        set(surface_material terrain)
    else()
        set(surface_material ground_detail)
    endif()
    set(action_result "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/${name}/result.json")
    hs_add_asset_action(
        NAME "hs_render_${name}" BUILDER hs_render_package
        RESULT "${action_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/${name}/result.d"
        STATIC_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/${surface_material}.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/material_graph.schema.json"
        UPSTREAM_RESULTS "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/meshes/result.json"
            "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/${surface_material}/result.json"
        ARGS --name "${name}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}")
    list(APPEND HS_GROUND_DETAIL_GEOMETRY_RESULTS "${action_result}")
    # Meadow surfaces have no eligible HLOD members in the environment layout.
    if(NOT name STREQUAL "meadow_ground" AND NOT name STREQUAL "meadow_water")
        list(APPEND HS_HLOD_GROUND_DETAIL_GEOMETRY_RESULTS "${action_result}")
    endif()
endforeach()
list(APPEND HS_RENDER_ACTION_RESULTS ${HS_GROUND_DETAIL_GEOMETRY_RESULTS})
set(HS_HERO_MATERIAL_RESULTS)
set(HS_SCENE_HLOD_RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/hlod/arena/result.json")
set(HS_SCENE_HLOD_UPSTREAMS "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/simulation-payload/result.json")
set(HS_SCENE_PROBE_UPSTREAMS
    "${HS_LIGHTING_PROBE_RESULT}"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/simulation-payload/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/terrain/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/terrain/parameters/result.json")
foreach(name IN LISTS HS_AUTHORED_MATERIAL_NAMES)
    if(name STREQUAL "terrain")
        continue()
    endif()
    list(APPEND HS_SCENE_PROBE_UPSTREAMS
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/${name}/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/${name}/parameters/result.json")
    if(NOT name MATCHES "^grass_")
        list(APPEND HS_SCENE_HLOD_UPSTREAMS "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/${name}/result.json")
    endif()
endforeach()
list(APPEND HS_SCENE_PROBE_UPSTREAMS ${HS_GROUND_DETAIL_GEOMETRY_RESULTS})
list(APPEND HS_SCENE_HLOD_UPSTREAMS ${HS_HLOD_GROUND_DETAIL_GEOMETRY_RESULTS})
hs_add_asset_action(NAME hs_render_scene_hlod BUILDER hs_render_scene_package
    RESULT "${HS_SCENE_HLOD_RESULT}"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/hlod/arena/result.d"
    STATIC_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/Source/Presentation/Private/environment_layout.cpp"
    UPSTREAM_RESULTS ${HS_SCENE_HLOD_UPSTREAMS}
    ARGS --source-root "${CMAKE_CURRENT_SOURCE_DIR}" --build-root "${CMAKE_CURRENT_BINARY_DIR}")
set(HS_SCENE_PROBE_RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/local-probes/result.json")
set(HS_SCENE_PROBE_RESOLVE_RESULTS
    ${HS_SCENE_PROBE_UPSTREAMS} ${HS_ENVIRONMENT_TEXTURE_RESULTS} ${HS_RENDER_TEXTURE_RESULTS}
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/meshes/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/histogram/result.json")
foreach(name IN LISTS HS_AUTHORED_MATERIAL_NAMES)
    list(APPEND HS_SCENE_PROBE_RESOLVE_RESULTS
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/${name}/result.json")
endforeach()
list(REMOVE_DUPLICATES HS_SCENE_PROBE_RESOLVE_RESULTS)
set(HS_SCENE_PROBE_RESULT_LIST "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/scene-probe-input-results.txt")
list(JOIN HS_SCENE_PROBE_RESOLVE_RESULTS "\n" HS_SCENE_PROBE_RESULT_LINES)
file(GENERATE OUTPUT "${HS_SCENE_PROBE_RESULT_LIST}" CONTENT "${HS_SCENE_PROBE_RESULT_LINES}\n")
hs_add_asset_action(NAME hs_render_scene_probes BUILDER hs_render_scene_probe
    RESULT "${HS_SCENE_PROBE_RESULT}"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/local-probes/result.d"
    STATIC_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Lighting/local_probes.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/scene_probe.schema.json"
        "${HS_SCENE_PROBE_RESULT_LIST}"
    UPSTREAM_RESULTS ${HS_SCENE_PROBE_RESOLVE_RESULTS}
    ARGS --source-root "${CMAKE_CURRENT_SOURCE_DIR}" --build-root "${CMAKE_CURRENT_BINARY_DIR}"
        --result-list "${HS_SCENE_PROBE_RESULT_LIST}")
foreach(name IN LISTS HS_HERO_MATERIAL_NAMES)
    set(hero_result "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/${name}/result.json")
    set(hero_parameters "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/${name}/parameters/result.json")
    set(hero_texture_results)
    if(name STREQUAL "monster_pbr")
        list(APPEND hero_texture_results
            "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/character/monster-materials/result.json")
    endif()
    hs_add_asset_action(
        NAME "hs_render_material_shader_${name}" BUILDER hs_render_material_shader
        RESULT "${hero_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/${name}/result.d"
        STATIC_INPUTS ${HS_DEFERRED_MATERIAL_INPUTS} ${HS_MATERIAL_SURFACE_LIBRARY}
            "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/material_graph.schema.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/hero_material.hlsli"
            "${HS_SHADER_SOURCE}" "${HS_GPU_SCENE_TABLES_SOURCE}" "${HS_GEOMETRY_SHADER_SOURCE}"
            ${HS_VISIBILITY_SHADER_SOURCE} ${HS_DXC_TOOL_INPUTS}
        UPSTREAM_RESULTS ${hero_texture_results}
        ARGS --name "${name}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}" --dxc "${HS_DXC}")
    hs_add_asset_action(
        NAME "hs_render_material_parameters_${name}" BUILDER hs_render_material_shader
        RESULT "${hero_parameters}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/${name}/parameters/result.d"
        STATIC_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/${name}.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/material_graph.schema.json"
        ARGS --parameters --name "${name}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}")
    list(APPEND HS_HERO_MATERIAL_RESULTS "${hero_result}" "${hero_parameters}")
    list(APPEND HS_RENDER_SHADER_RESULTS "${hero_result}")
    list(APPEND HS_RENDER_ACTION_RESULTS "${hero_parameters}")
endforeach()
# Decal programs have an additive domain and independent material ordinals.
# Defaults can change without invalidating the combined Surface program.
set(HS_DECAL_MATERIAL_NAMES decal_fracture decal_organic)
set(HS_DECAL_MATERIAL_RESULTS
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/decal-surface/result.json")
set(HS_DECAL_MATERIAL_SOURCES)
foreach(name IN LISTS HS_DECAL_MATERIAL_NAMES)
    set(decal_source "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/${name}.json")
    set(decal_parameters "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/${name}/parameters/result.json")
    list(APPEND HS_DECAL_MATERIAL_SOURCES "${decal_source}")
    list(APPEND HS_DECAL_MATERIAL_RESULTS "${decal_parameters}")
    hs_add_asset_action(
        NAME "hs_render_material_parameters_${name}" BUILDER hs_render_material_shader
        RESULT "${decal_parameters}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/${name}/parameters/result.d"
        STATIC_INPUTS "${decal_source}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/decal_material_graph.schema.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/material_graph.schema.json"
        ARGS --parameters --name "${name}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()
hs_add_asset_action(
    NAME hs_render_material_shader_decal_surface BUILDER hs_render_material_shader
    RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/decal-surface/result.json"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/decal-surface/result.d"
    STATIC_INPUTS ${HS_DECAL_MATERIAL_SOURCES}
        "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/decal_material_graph.schema.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/material_graph.schema.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/decal_surface.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_surface.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_parameters.hlsli"
        ${HS_DXC_TOOL_INPUTS}
    ARGS --decal-surface --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --build-root "${CMAKE_CURRENT_BINARY_DIR}" --dxc "${HS_DXC}")
add_executable(hs_character_content
    Source/Apps/CharacterContent/main.cpp
    Source/Apps/Content/character_asset_cooker.cpp
    Source/Apps/Content/character_products.cpp
    Source/Apps/Content/content_paths.cpp)
target_link_libraries(hs_character_content PRIVATE hs_asset_pipeline)
target_include_directories(hs_character_content PRIVATE Source/Apps/Content)
target_include_directories(hs_character_content SYSTEM PRIVATE "${HS_FBX_SDK_ROOT}/include")
target_link_libraries(hs_character_content PRIVATE
    hs_core hs_render_data hs_geometry_cooker hs_game_rules hs_presentation nlohmann_json::nlohmann_json
    DirectXTex fbxsdk fbx_xml fbx_zlib ws2_32 windowscodecs d3d11 ole32 bcrypt)
target_compile_definitions(hs_character_content PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_character_content)
set(HS_CHARACTER_ACTION_RESULTS)
set(HS_CHARACTER_MODES archer slime-melee slime-ranged slime-suicide slime-projectile
    boss-5m boss-10m boss-final)
set(HS_CHARACTER_STEMS archer enemy_melee enemy_ranged enemy_suicide enemy_gel_projectile
    boss_5m boss_10m boss_final)
foreach(mode IN LISTS HS_CHARACTER_MODES ITEMS monster-materials)
    if(mode STREQUAL "archer")
        set(action_inputs
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Characters/Archer/ErikaArcher.fbx")
    elseif(mode STREQUAL "monster-materials")
        set(action_inputs ${HS_MONSTER_TEXTURE_SOURCES})
    elseif(mode MATCHES "^slime-")
        if(mode STREQUAL "slime-projectile")
            set(variant Projectile)
            set(stem SlimeGelProjectile)
        elseif(mode STREQUAL "slime-melee")
            set(variant Melee)
            set(stem SlimeMelee)
        elseif(mode STREQUAL "slime-ranged")
            set(variant Ranged)
            set(stem SlimeRanged)
        else()
            set(variant Suicide)
            set(stem SlimeSuicide)
        endif()
        set(action_inputs
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Monsters/SlimeFamily/${variant}/${stem}.fbx")
    elseif(mode STREQUAL "boss-5m")
        set(action_inputs
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Monsters/TurtleShell_SK.fbx")
    elseif(mode STREQUAL "boss-10m")
        set(action_inputs
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Monsters/ChestMonster_SK.fbx")
    else()
        set(action_inputs
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Monsters/Beholder_SK.fbx")
    endif()
    hs_add_asset_action(
        NAME "hs_character_${mode}"
        BUILDER hs_character_content
        RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/character/${mode}/result.json"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/character/${mode}/result.d"
        STATIC_INPUTS ${action_inputs}
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/hero_bindings.json"
        ARGS "--${mode}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}")
    list(APPEND HS_CHARACTER_ACTION_RESULTS
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/character/${mode}/result.json")
endforeach()
add_custom_target(hs_character_products DEPENDS ${HS_CHARACTER_ACTION_RESULTS})
add_executable(hs_render_character_texture_package
    Source/Apps/RenderCharacterTexturePackage/main.cpp)
target_link_libraries(hs_render_character_texture_package PRIVATE hs_asset_pipeline)
target_link_libraries(hs_render_character_texture_package PRIVATE
    hs_render_data hs_core nlohmann_json::nlohmann_json bcrypt)
target_compile_definitions(hs_render_character_texture_package PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_render_character_texture_package)
set(HS_CHARACTER_TEXTURE_RESULTS)
foreach(mode IN ITEMS archer slime-melee slime-ranged slime-suicide slime-projectile
        boss-5m boss-10m boss-final monster-materials)
    set(texture_result
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/character/${mode}/result.json")
    hs_add_asset_action(
        NAME "hs_render_character_texture_${mode}"
        BUILDER hs_render_character_texture_package
        RESULT "${texture_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/character/${mode}/result.d"
        UPSTREAM_RESULTS "${CMAKE_CURRENT_BINARY_DIR}/Actions/character/${mode}/result.json"
        ARGS --mode "${mode}" --build-root "${CMAKE_CURRENT_BINARY_DIR}")
    list(APPEND HS_CHARACTER_TEXTURE_RESULTS "${texture_result}")
endforeach()
add_custom_target(hs_render_character_texture_products DEPENDS ${HS_CHARACTER_TEXTURE_RESULTS})
set(HS_CHARACTER_LINK_ARGUMENTS)
foreach(mode stem IN ZIP_LISTS HS_CHARACTER_MODES HS_CHARACTER_STEMS)
    list(APPEND HS_CHARACTER_LINK_ARGUMENTS --character
        "character/${mode}" "render/character/${stem}.rpkg")
endforeach()
list(APPEND HS_CHARACTER_LINK_ARGUMENTS --binding-result character/monster-materials)
set(HS_CHARACTER_BINDING_RESULTS ${HS_CHARACTER_TEXTURE_RESULTS})
foreach(mode IN LISTS HS_CHARACTER_MODES ITEMS monster-materials)
    list(APPEND HS_CHARACTER_LINK_ARGUMENTS --binding-result "render/texture/character/${mode}")
endforeach()
foreach(name IN LISTS HS_HERO_MATERIAL_NAMES)
    list(APPEND HS_CHARACTER_LINK_ARGUMENTS --binding-result "render/shader/${name}")
    list(APPEND HS_CHARACTER_BINDING_RESULTS
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/${name}/result.json")
endforeach()
add_executable(hs_vfx_content
    Source/Apps/VfxContent/main.cpp
    Source/Apps/VfxContent/particle_bridge.cpp
    Source/Apps/Content/particle_effect_cooker.cpp
    Source/Apps/Content/vfx_mesh_cooker.cpp
    Source/Apps/Content/content_paths.cpp)
target_link_libraries(hs_vfx_content PRIVATE hs_asset_pipeline)
target_include_directories(hs_vfx_content PRIVATE Source/Apps/Content)
target_link_libraries(hs_vfx_content PRIVATE
    hs_vfx_authoring hs_core hs_game_rules hs_presentation
    nlohmann_json::nlohmann_json DirectXTex ole32 bcrypt)
target_compile_definitions(hs_vfx_content PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_vfx_content)
foreach(mode IN ITEMS particle-definitions masks vfx-runtime mesh-atlas)
    if(mode STREQUAL "particle-definitions" OR mode STREQUAL "masks")
        set(action_inputs
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/GameData/particles.json")
    elseif(mode STREQUAL "vfx-runtime")
        set(action_inputs
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/VFX/production_spec.v4.json")
    else()
        set(action_inputs
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/VFX/Meshes/vfx_mesh_atlas.json")
    endif()
    hs_add_asset_action(
        NAME "hs_vfx_${mode}"
        BUILDER hs_vfx_content
        RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/${mode}/result.json"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/${mode}/result.d"
        STATIC_INPUTS ${action_inputs}
        ARGS "--${mode}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()
add_custom_target(hs_vfx_products
    DEPENDS
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/particle-definitions/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/masks/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/vfx-runtime/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/mesh-atlas/result.json")
add_executable(hs_render_vfx_texture_package
    Source/Apps/RenderVfxTexturePackage/main.cpp)
target_link_libraries(hs_render_vfx_texture_package PRIVATE hs_asset_pipeline)
target_link_libraries(hs_render_vfx_texture_package PRIVATE
    hs_render_data hs_core nlohmann_json::nlohmann_json bcrypt)
target_compile_definitions(hs_render_vfx_texture_package PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_render_vfx_texture_package)
set(HS_RENDER_VFX_TEXTURE_RESULTS)
foreach(texture_name IN ITEMS
        vfx_masks vfx_gradient_lut vfx_ribbon_detail_array vfx_authored_mask_array
        vfx_noise_basis_2d vfx_noise_volume_64 vfx_curve_lut vfx_smoke6way_pos vfx_smoke6way_neg
        vfx_smoke_motion_vectors vfx_flow_curl_2d vfx_stbn_scalar
        vfx_fracture_decal_array vfx_organic_decal_array)
    if(texture_name STREQUAL "vfx_masks")
        set(texture_upstream "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/masks/result.json")
    else()
        set(texture_upstream "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/vfx-runtime/result.json")
    endif()
    set(texture_result
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/${texture_name}/result.json")
    hs_add_asset_action(
        NAME "hs_render_texture_${texture_name}"
        BUILDER hs_render_vfx_texture_package
        RESULT "${texture_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/${texture_name}/result.d"
        UPSTREAM_RESULTS "${texture_upstream}"
        ARGS --build-root "${CMAKE_CURRENT_BINARY_DIR}" --name "${texture_name}")
    list(APPEND HS_RENDER_VFX_TEXTURE_RESULTS "${texture_result}")
endforeach()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-audio.json"
    COMMAND "$<TARGET_FILE:hs_generation>" --compose
        --build-root "${CMAKE_CURRENT_BINARY_DIR}"
        --scope test-audio
        --result "${CMAKE_CURRENT_BINARY_DIR}/Actions/audio/result.json"
    DEPENDS hs_generation "${CMAKE_CURRENT_BINARY_DIR}/Actions/audio/result.json"
    VERBATIM
)
add_custom_target(hs_audio_generation
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-audio.json")
set(HS_SIMULATION_FINALIZER_RESULTS
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/simulation-payload/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/gameplay-identity/result.json")
set(HS_PRESENTATION_FINALIZER_RESULTS
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/presentation-payload/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/content-identity/result.json")
function(hs_add_test_generation name)
    cmake_parse_arguments(A "" "" "RESULTS;REQUIRED_IDS" ${ARGN})
    if(NOT A_RESULTS OR A_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "hs_add_test_generation(${name}) requires RESULTS")
    endif()
    set(required_args)
    foreach(logical_id IN LISTS A_REQUIRED_IDS)
        list(APPEND required_args --require "${logical_id}")
    endforeach()
    set(scope "test-${name}")
    set(pointer "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/${scope}.json")
    set(result_list "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/${scope}-results.txt")
    list(JOIN A_RESULTS "\n" result_lines)
    file(GENERATE OUTPUT "${result_list}" CONTENT "${result_lines}\n")
    add_custom_command(
        OUTPUT "${pointer}"
        COMMAND "$<TARGET_FILE:hs_generation>" --compose
            --build-root "${CMAKE_CURRENT_BINARY_DIR}" --scope "${scope}"
            --result-list "${result_list}" ${required_args}
        DEPENDS hs_generation "${result_list}" ${A_RESULTS}
        VERBATIM)
    add_custom_target("hs_test_${name}_generation" DEPENDS "${pointer}")
endfunction()
hs_add_test_generation(gameplay RESULTS
    ${HS_SIMULATION_FINALIZER_RESULTS}
    ${HS_PRESENTATION_FINALIZER_RESULTS}
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/finalize-simulation/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/finalize-presentation/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/character/slime-melee/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/character/slime-ranged/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/character/slime-suicide/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/character/slime-projectile/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/character/slime-melee/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/character/slime-ranged/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/character/slime-suicide/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/character/slime-projectile/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/slime_features/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/slime_body/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/slime_features/parameters/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/slime_body/parameters/result.json")
hs_add_test_generation(vfx RESULTS
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/vfx-runtime/result.json"
    REQUIRED_IDS vfx/vfx_program.hsbin)
hs_add_test_generation(runtime RESULTS
    ${HS_SIMULATION_FINALIZER_RESULTS}
    ${HS_PRESENTATION_FINALIZER_RESULTS}
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/finalize-simulation/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/finalize-presentation/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/vfx-runtime/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/particle-definitions/result.json"
    REQUIRED_IDS vfx/vfx_program.hsbin vfx/particle_effects.hsbin
        game-data/simulation-rules game-data/presentation-catalog
        game-data/simulation-payload game-data/gameplay-identity
        game-data/presentation-payload game-data/content-identity)
hs_add_test_generation(environment RESULTS
    "${HS_QUALITY_PROFILES_RESULT}"
    "${HS_LIGHTING_PROBE_RESULT}"
    "${HS_LTC_LUT_RESULT}"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/histogram/result.json"
    ${HS_ENVIRONMENT_TEXTURE_RESULTS}
    ${HS_RENDER_TEXTURE_RESULTS}
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/meshes/result.json"
    ${HS_RENDER_SHADER_RESULTS}
    ${HS_RENDER_ACTION_RESULTS}
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/character/monster-materials/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/character/monster-materials/result.json")
hs_add_test_generation(character RESULTS
    "${HS_QUALITY_PROFILES_RESULT}"
    ${HS_CHARACTER_ACTION_RESULTS}
    ${HS_CHARACTER_TEXTURE_RESULTS}
    ${HS_HERO_MATERIAL_RESULTS})
hs_add_test_generation(combat RESULTS
    ${HS_SIMULATION_FINALIZER_RESULTS}
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/finalize-simulation/result.json"
    REQUIRED_IDS
        game-data/simulation-rules
        game-data/simulation-payload
        game-data/gameplay-identity)
set(HS_RUNTIME_FULL_RESULTS
    "${HS_QUALITY_PROFILES_RESULT}"
    "${HS_VOLUMETRIC_MEDIA_RESULT}"
    "${HS_SCENE_HLOD_RESULT}"
    "${HS_SCENE_PROBE_RESULT}"
    "${HS_LIGHTING_PROBE_RESULT}"
    "${HS_LTC_LUT_RESULT}"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/audio/result.json"
    ${HS_SIMULATION_FINALIZER_RESULTS}
    ${HS_PRESENTATION_FINALIZER_RESULTS}
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/finalize-simulation/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/finalize-presentation/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/histogram/result.json"
    ${HS_ENVIRONMENT_TEXTURE_RESULTS}
    ${HS_RENDER_TEXTURE_RESULTS}
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/meshes/result.json"
    ${HS_RENDER_SHADER_RESULTS}
    ${HS_RENDER_ACTION_RESULTS}
    ${HS_DECAL_MATERIAL_RESULTS}
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/particle-definitions/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/masks/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/vfx-runtime/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/mesh-atlas/result.json"
    ${HS_RENDER_VFX_TEXTURE_RESULTS}
    ${HS_CHARACTER_ACTION_RESULTS}
    ${HS_CHARACTER_TEXTURE_RESULTS})
set(HS_RENDER_TEST_RESULTS ${HS_RUNTIME_FULL_RESULTS})
list(REMOVE_ITEM HS_RENDER_TEST_RESULTS
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/audio/result.json"
    ${HS_PRESENTATION_FINALIZER_RESULTS}
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/finalize-presentation/result.json")
hs_add_test_generation(render RESULTS ${HS_RENDER_TEST_RESULTS})
if(TARGET hs_render_test_runner)
    if(TARGET hs_static_shadow_bake)
    # Keep the bake input independent of its output; runtime quality is not selected here.
    set(HS_STATIC_SHADOW_INPUT_LIST
        "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/static-shadow-input-results.txt")
    set(HS_STATIC_SHADOW_INPUT
        "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/static-shadow-input.json")
    list(JOIN HS_RENDER_TEST_RESULTS "\n" HS_STATIC_SHADOW_INPUT_LINES)
    file(GENERATE OUTPUT "${HS_STATIC_SHADOW_INPUT_LIST}"
        CONTENT "${HS_STATIC_SHADOW_INPUT_LINES}\n")
    add_custom_command(
        OUTPUT "${HS_STATIC_SHADOW_INPUT}"
        COMMAND "$<TARGET_FILE:hs_generation>" --compose
            --build-root "${CMAKE_CURRENT_BINARY_DIR}" --scope static-shadow-input
            --result-list "${HS_STATIC_SHADOW_INPUT_LIST}"
        DEPENDS hs_generation "${HS_STATIC_SHADOW_INPUT_LIST}" ${HS_RENDER_TEST_RESULTS}
        VERBATIM)
    hs_add_asset_action(NAME hs_render_static_shadow_candidate BUILDER hs_static_shadow_bake
        RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/arena-static-shadow/result.json"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/arena-static-shadow/result.d"
        STATIC_INPUTS "${HS_STATIC_SHADOW_INPUT}" "${HS_STATIC_SHADOW_INPUT_LIST}"
        ARGS --produce-static-shadow --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}" --generation "${HS_STATIC_SHADOW_INPUT}"
            --result-list "${HS_STATIC_SHADOW_INPUT_LIST}")
    list(APPEND HS_RUNTIME_FULL_RESULTS
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/arena-static-shadow/result.json")
    endif()

    if(TARGET hs_scene_probe_bake)
    # Bake from the layout-only selection. The output Action replaces that
    # product only in runtime-full, after static-shadow input has been formed.
    set(HS_SCENE_PROBE_BAKE_INPUT_LIST
        "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/scene-probe-bake-input-results.txt")
    set(HS_SCENE_PROBE_BAKE_INPUT
        "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/scene-probe-bake-input.json")
    list(JOIN HS_RENDER_TEST_RESULTS "\n" HS_SCENE_PROBE_BAKE_INPUT_LINES)
    file(GENERATE OUTPUT "${HS_SCENE_PROBE_BAKE_INPUT_LIST}"
        CONTENT "${HS_SCENE_PROBE_BAKE_INPUT_LINES}\n")
    add_custom_command(
        OUTPUT "${HS_SCENE_PROBE_BAKE_INPUT}"
        COMMAND "$<TARGET_FILE:hs_generation>" --compose
            --build-root "${CMAKE_CURRENT_BINARY_DIR}" --scope scene-probe-bake-input
            --result-list "${HS_SCENE_PROBE_BAKE_INPUT_LIST}"
        DEPENDS hs_generation hs_scene_probe_bake "${HS_SCENE_PROBE_BAKE_INPUT_LIST}" ${HS_RENDER_TEST_RESULTS}
        VERBATIM)
    set(HS_SCENE_PROBE_BAKED_RESULT
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/scene-probes-baked/result.json")
    hs_add_asset_action(NAME hs_render_scene_probe_bake BUILDER hs_scene_probe_bake
        RESULT "${HS_SCENE_PROBE_BAKED_RESULT}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/scene-probes-baked/result.d"
        STATIC_INPUTS "${HS_SCENE_PROBE_BAKE_INPUT}" "${HS_SCENE_PROBE_BAKE_INPUT_LIST}"
        ARGS --produce-scene-probe --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}" --generation "${HS_SCENE_PROBE_BAKE_INPUT}"
            --result-list "${HS_SCENE_PROBE_BAKE_INPUT_LIST}")
    add_dependencies(hs_render_scene_probe_bake hs_render_static_shadow_candidate)
    list(REMOVE_ITEM HS_RUNTIME_FULL_RESULTS "${HS_SCENE_PROBE_RESULT}")
    list(APPEND HS_RUNTIME_FULL_RESULTS "${HS_SCENE_PROBE_BAKED_RESULT}")
    endif()
endif()
if(HS_BUILD_TESTS)
    # The full verify workflow builds ALL before running CTest directly.
    # Exact test_prep targets still build only their selected scope.
    set(HS_TEST_ALL_GENERATION_TARGETS
        hs_audio_generation hs_test_gameplay_generation hs_test_environment_generation
        hs_test_character_generation hs_test_combat_generation)
    if(HS_BUILD_RUNTIME)
        list(APPEND HS_TEST_ALL_GENERATION_TARGETS
            hs_test_vfx_generation hs_test_runtime_generation
            hs_test_render_generation)
    endif()
    add_custom_target(hs_test_generations ALL
        DEPENDS ${HS_TEST_ALL_GENERATION_TARGETS})
endif()
set(HS_RUNTIME_FULL_RESULT_LIST
    "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/runtime-full-results.txt")
list(JOIN HS_RUNTIME_FULL_RESULTS "\n" HS_RUNTIME_FULL_RESULT_LINES)
file(GENERATE OUTPUT "${HS_RUNTIME_FULL_RESULT_LIST}"
    CONTENT "${HS_RUNTIME_FULL_RESULT_LINES}\n")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Shipping/link.stamp"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/Shipping/content.generated.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/Shipping/content.receipt.json"
        "${CMAKE_CURRENT_BINARY_DIR}/${HS_SHIPPING_PACK_RELATIVE_PATH}"
    COMMAND "$<TARGET_FILE:hs_content_linker>" --build-root "${CMAKE_CURRENT_BINARY_DIR}"
        --font "${HS_FONT_DIRECTORY}/NotoSansKR.ttf" ${HS_CHARACTER_LINK_ARGUMENTS}
        --renderer-result-list "${HS_RUNTIME_FULL_RESULT_LIST}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/Shipping/link.stamp"
    DEPENDS hs_content_linker "${HS_FONT_DIRECTORY}/NotoSansKR.ttf"
        "${HS_RUNTIME_FULL_RESULT_LIST}" ${HS_RUNTIME_FULL_RESULTS}
        ${HS_CHARACTER_ACTION_RESULTS} ${HS_CHARACTER_BINDING_RESULTS}
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/presentation-payload/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/simulation-payload/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/content-identity/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/audio/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/particle-definitions/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/vfx/vfx-runtime/result.json"
    COMMAND_EXPAND_LISTS VERBATIM)
add_custom_target(hs_shipping_image
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/Shipping/link.stamp")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/runtime-full.json"
    COMMAND "$<TARGET_FILE:hs_generation>" --compose
        --build-root "${CMAKE_CURRENT_BINARY_DIR}"
        --scope runtime-full
        --result-list "${HS_RUNTIME_FULL_RESULT_LIST}"
    DEPENDS hs_generation "${HS_RUNTIME_FULL_RESULT_LIST}"
        ${HS_RUNTIME_FULL_RESULTS}
    VERBATIM)
set(HS_CONTENT_GENERATION_DEFAULT_ALL)
if(HS_BUILD_RUNTIME)
    set(HS_CONTENT_GENERATION_DEFAULT_ALL ALL)
endif()
add_custom_target(hs_content_generation ${HS_CONTENT_GENERATION_DEFAULT_ALL}
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/runtime-full.json")
# Compatibility target name; the split Actions are the only production authority.
add_custom_target(hs_cooked_content DEPENDS hs_content_generation)
if(HS_BUILD_RUNTIME AND HS_SINGLE_CONFIG_RELEASE)
    add_dependencies(ProjectHS hs_content_generation)
endif()
endif()
if(HS_BUILD_RUNTIME AND NOT HS_BUILD_CONTENT_TOOLS AND NOT HS_SINGLE_CONFIG_RELEASE)
    add_custom_target(hs_content_generation ALL
        COMMAND "$<TARGET_FILE:hs_generation>" --verify
            --build-root "${CMAKE_CURRENT_BINARY_DIR}" --scope runtime-full
        DEPENDS hs_generation
        VERBATIM)
endif()

if(HS_BUILD_RUNTIME AND (HS_BUILD_CONTENT_TOOLS OR HS_SINGLE_CONFIG_RELEASE))
    add_library(hs_shipping_content STATIC EXCLUDE_FROM_ALL "${CMAKE_CURRENT_BINARY_DIR}/Shipping/content.generated.cpp")
    target_link_libraries(hs_shipping_content PRIVATE hs_runtime_contracts)
    target_link_libraries(hs_shipping_content PUBLIC hs_presentation hs_game_rules hs_render_data)
    if(HS_BUILD_CONTENT_TOOLS)
        add_dependencies(hs_shipping_content hs_shipping_image)
    endif()
    hs_configure_target(hs_shipping_content)
    target_link_libraries(ProjectHS PRIVATE $<$<CONFIG:Release>:hs_shipping_content>)
    target_link_libraries(hs_experiment PRIVATE $<$<CONFIG:Release>:hs_shipping_content>)
endif()

if(HS_BUILD_TESTS)
    set(CMAKE_TEST_BUILD_DEPENDS ON)
    enable_testing()
    if(HS_BUILD_RUNTIME)
        add_test(NAME build.release-install-transaction COMMAND "${CMAKE_COMMAND}"
            "-DHS_TEST_WORK_ROOT=${CMAKE_CURRENT_BINARY_DIR}/Tests/release-install-transaction"
            "-DHS_TEST_ALLOWED_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
            "-DHS_TEST_GENERATOR=${CMAKE_GENERATOR}"
            "-DHS_TEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_release_install.cmake")
        set_tests_properties(build.release-install-transaction PROPERTIES TIMEOUT 90)
    endif()
    add_executable(hs_tests Tests/foundation_tests.cpp Tests/shipped_content_tests.cpp)
    target_link_libraries(hs_tests PRIVATE hs_core hs_jobs)
    hs_configure_target(hs_tests)

    add_test(NAME foundation.core COMMAND hs_tests foundation)
    add_executable(hs_render_scene_extractor_tests
        Tests/render_scene_extractor_tests.cpp)
    target_link_libraries(hs_render_scene_extractor_tests PRIVATE hs_core hs_render_data_values hs_presentation)
    target_include_directories(hs_render_scene_extractor_tests PRIVATE
        Source/RendererD3D12/Private)
    hs_configure_target(hs_render_scene_extractor_tests)
    add_test(NAME rendering.scene-sample
        COMMAND hs_render_scene_extractor_tests)
    add_executable(hs_hzb_region_tests Tests/hzb_region_tests.cpp)
    hs_configure_target(hs_hzb_region_tests)
    add_test(NAME rendering.hzb-region-cpu COMMAND hs_hzb_region_tests)
    if(HS_BUILD_RUNTIME)
        set(HS_HZB_PROBE_SHADER "${CMAKE_CURRENT_BINARY_DIR}/Generated/TestShaders/hzb_region.dxil")
        add_custom_command(OUTPUT "${HS_HZB_PROBE_SHADER}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/Generated/TestShaders"
            COMMAND "${HS_DXC}" -nologo -WX -O3 -Ges -E main -T cs_6_0
                -I "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders"
                -Fo "${HS_HZB_PROBE_SHADER}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/hzb_region_probe.hlsl"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/hzb_region_probe.hlsl"
                "${HS_HZB_REGION_SOURCE}" ${HS_DXC_TOOL_INPUTS}
            VERBATIM)
        add_executable(hs_hzb_region_gpu_tests Tests/hzb_region_tests.cpp "${HS_HZB_PROBE_SHADER}")
        target_link_libraries(hs_hzb_region_gpu_tests PRIVATE d3d12 dxgi)
        target_include_directories(hs_hzb_region_gpu_tests BEFORE PRIVATE "${agility_SOURCE_DIR}/build/native/include")
        target_compile_definitions(hs_hzb_region_gpu_tests PRIVATE HS_HZB_GPU_PROBE=1 WIN32_LEAN_AND_MEAN NOMINMAX)
        hs_configure_target(hs_hzb_region_gpu_tests)
        add_dependencies(hs_hzb_region_gpu_tests hs_runtime_native_assets)
        add_test(NAME rendering.hzb-region-gpu
            COMMAND hs_hzb_region_gpu_tests "${HS_HZB_PROBE_SHADER}"
            BUILD_DEPENDS hs_hzb_region_gpu_tests)
        add_test(NAME rendering.hzb-region-warp
            COMMAND hs_hzb_region_gpu_tests "${HS_HZB_PROBE_SHADER}" --warp
            BUILD_DEPENDS hs_hzb_region_gpu_tests)
        set_tests_properties(rendering.hzb-region-gpu rendering.hzb-region-warp
            PROPERTIES TIMEOUT 90 RESOURCE_LOCK projecths_d3d12)
    endif()
    add_executable(hs_vfx_session_state_tests Tests/vfx_session_state_tests.cpp
        Tests/vfx_profile_admission_tests.cpp Tests/vfx_image_header.cpp)
    target_link_libraries(hs_vfx_session_state_tests PRIVATE hs_runtime_contracts)
    target_include_directories(hs_vfx_session_state_tests PRIVATE
        Source/Runtime/Private)
    hs_configure_target(hs_vfx_session_state_tests)
    add_test(NAME runtime.vfx-session-state COMMAND hs_vfx_session_state_tests)
    # Isolate global allocation failure injection from ordinary VFX contracts.
    add_executable(hs_vfx_session_state_failure_tests Tests/vfx_session_state_failure_tests.cpp)
    target_link_libraries(hs_vfx_session_state_failure_tests PRIVATE hs_runtime_contracts)
    target_include_directories(hs_vfx_session_state_failure_tests PRIVATE Source/Runtime/Private)
    hs_configure_target(hs_vfx_session_state_failure_tests)
    add_test(NAME runtime.vfx-session-state-allocation-failure COMMAND hs_vfx_session_state_failure_tests)
    set_tests_properties(runtime.vfx-session-state-allocation-failure PROPERTIES TIMEOUT 30)
    add_executable(hs_ui_command_admission_tests Tests/ui_command_admission_tests.cpp)
    target_link_libraries(hs_ui_command_admission_tests PRIVATE hs_presentation)
    hs_configure_target(hs_ui_command_admission_tests)
    add_test(NAME runtime.ui-command-admission COMMAND hs_ui_command_admission_tests)
    add_executable(hs_runtime_diagnostics_tests Tests/runtime_diagnostics_tests.cpp)
    hs_configure_target(hs_runtime_diagnostics_tests)
    add_test(NAME runtime.diagnostics-bounds COMMAND hs_runtime_diagnostics_tests)
    add_executable(hs_project_hs_arguments_tests
        Tests/project_hs_arguments_tests.cpp
        Source/Apps/ProjectHS/command_line.cpp)
    target_link_libraries(hs_project_hs_arguments_tests PRIVATE hs_runtime_contracts)
    hs_configure_target(hs_project_hs_arguments_tests)
    add_test(NAME runtime.arguments COMMAND hs_project_hs_arguments_tests)

    add_executable(hs_sha256_tests Tests/sha256_tests.cpp)
    target_link_libraries(hs_sha256_tests PRIVATE hs_core)
    hs_configure_target(hs_sha256_tests)
    add_test(NAME foundation.sha256 COMMAND hs_sha256_tests)
    if(HS_BUILD_RUNTIME OR HS_BUILD_RENDER_DATA_TESTS)
        add_executable(hs_vfx_typed_frame_tests
            Tests/vfx_typed_frame_adapter_tests.cpp
            Source/Runtime/Private/vfx_typed_frame_adapter.cpp
            Source/Runtime/Private/vfx_typed_screen_commands.cpp
            Source/Runtime/Private/vfx_typed_volume_commands.cpp)
        target_link_libraries(hs_vfx_typed_frame_tests PRIVATE hs_runtime_contracts)
        target_include_directories(hs_vfx_typed_frame_tests PRIVATE
            Source/Runtime/Private Source/AssetManifest/Public)
        hs_configure_target(hs_vfx_typed_frame_tests)
        add_test(NAME runtime.vfx-typed-frame COMMAND hs_vfx_typed_frame_tests)
        # DirectXMath comes from the Windows SDK; these tests create no device.
        if(WIN32)
            add_executable(hs_persistent_gpu_scene_tests
                Tests/persistent_gpu_scene_tests.cpp
                Source/RendererD3D12/Private/persistent_gpu_scene.cpp)
            target_link_libraries(hs_persistent_gpu_scene_tests PRIVATE hs_core hs_render_data_values)
            target_include_directories(hs_persistent_gpu_scene_tests PRIVATE
                Source/RendererD3D12/Private)
            hs_configure_target(hs_persistent_gpu_scene_tests)
            add_test(NAME rendering.persistent-gpu-scene
                COMMAND hs_persistent_gpu_scene_tests)
        endif()
        if(WIN32 AND HS_BUILD_RUNTIME)
            add_executable(hs_brixelizer_scene_input_tests Tests/brixelizer_scene_input_tests.cpp)
            target_link_libraries(hs_brixelizer_scene_input_tests PRIVATE hs_renderer_d3d12 hs_render_data)
            target_include_directories(hs_brixelizer_scene_input_tests PRIVATE Source/RendererD3D12/Private)
            target_compile_definitions(hs_brixelizer_scene_input_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
            hs_configure_target(hs_brixelizer_scene_input_tests)
            add_test(NAME rendering.brixelizer-scene-inputs COMMAND hs_brixelizer_scene_input_tests)
        endif()
        add_executable(hs_render_feature_tests
            Tests/render_feature_tests.cpp
            Source/RendererD3D12/Private/render_features.cpp)
        target_link_libraries(hs_render_feature_tests PRIVATE hs_core hs_render_data_values)
        target_include_directories(hs_render_feature_tests PRIVATE Source/RendererD3D12/Private)
        hs_configure_target(hs_render_feature_tests)
        add_test(NAME rendering.feature-signals COMMAND hs_render_feature_tests)
        add_executable(hs_dynamic_resolution_tests
            Tests/dynamic_resolution_tests.cpp
            Source/RendererD3D12/Private/view_state.cpp
            Source/RendererD3D12/Private/reconstruction_plan.cpp)
        target_link_libraries(hs_dynamic_resolution_tests PRIVATE hs_core hs_render_data_values)
        target_include_directories(hs_dynamic_resolution_tests PRIVATE
            Source/RendererD3D12/Private Source/RendererD3D12/Public)
        hs_configure_target(hs_dynamic_resolution_tests)
        add_test(NAME rendering.dynamic-resolution COMMAND hs_dynamic_resolution_tests)
        add_executable(hs_render_instance_order_tests Tests/render_instance_order_tests.cpp)
        target_link_libraries(hs_render_instance_order_tests PRIVATE hs_core hs_render_data_values)
        target_include_directories(hs_render_instance_order_tests PRIVATE Source/RendererD3D12/Private)
        hs_configure_target(hs_render_instance_order_tests)
        add_test(NAME rendering.instance-order COMMAND hs_render_instance_order_tests)
        add_executable(hs_renderer_caster_storage_tests Tests/renderer_caster_storage_tests.cpp)
        target_include_directories(hs_renderer_caster_storage_tests PRIVATE Source/RendererD3D12/Private)
        hs_configure_target(hs_renderer_caster_storage_tests)
        add_test(NAME rendering.caster-storage COMMAND hs_renderer_caster_storage_tests)
        add_executable(hs_local_probe_draw_tests Tests/local_probe_draw_tests.cpp)
        target_link_libraries(hs_local_probe_draw_tests PRIVATE hs_core hs_render_data_values)
        target_include_directories(hs_local_probe_draw_tests PRIVATE Source/RendererD3D12/Private)
        hs_configure_target(hs_local_probe_draw_tests)
        add_test(NAME rendering.local-probe-draws COMMAND hs_local_probe_draw_tests)
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME rendering.frame-constant-abi
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_shader_reconstruction.py")
            add_test(NAME rendering.light-list-capture-reader
                COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_light_shadow_lookup.py")
            add_test(NAME rendering.graphics-option-cost-selfcheck
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/analyze-graphics-options.py" --self-test)
            add_test(NAME rendering.play-capture-selfcheck
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/analyze-play-capture.py" --self-test)
            add_test(NAME rendering.oit-capture-analyzer-selfcheck
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/Tests/analyze_oit_overdraw.py")
        endif()
    endif()
    if(HS_BUILD_ASSET_PIPELINE_TESTS AND CMAKE_GENERATOR MATCHES "Ninja")
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)


            add_executable(hs_generation_reader_probe Tests/generation_reader_probe.cpp)
            target_link_libraries(hs_generation_reader_probe PRIVATE hs_asset_manifest)
            hs_configure_target(hs_generation_reader_probe)
            add_test(NAME content.generation-lifetime
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_generation_lifetime.py"
                    --generation "$<TARGET_FILE:hs_generation>"
                    --probe "$<TARGET_FILE:hs_generation_reader_probe>"
                    --audio "$<TARGET_FILE:hs_audio_content>"
                BUILD_DEPENDS hs_generation hs_generation_reader_probe hs_audio_content)
            add_test(NAME content.asset-pipeline-portable
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_asset_pipeline_portable.py"
                    --audio "$<TARGET_FILE:hs_audio_content>"
                    --generation "$<TARGET_FILE:hs_generation>"
                    --cmake "${CMAKE_COMMAND}" --ninja "${CMAKE_MAKE_PROGRAM}"
                BUILD_DEPENDS hs_audio_content hs_generation)
        endif()
    endif()
    add_executable(hs_graphics_settings_tests Tests/graphics_settings_tests.cpp)
    target_link_libraries(hs_graphics_settings_tests PRIVATE hs_core)
    hs_configure_target(hs_graphics_settings_tests)
    add_test(NAME foundation.graphics-settings COMMAND hs_graphics_settings_tests)
    add_test(NAME foundation.task-system COMMAND hs_tests jobs)
    add_executable(hs_task_system_failure_tests Tests/task_system_failure_tests.cpp)
    target_link_libraries(hs_task_system_failure_tests PRIVATE hs_jobs Taskflow::Taskflow)
    hs_configure_target(hs_task_system_failure_tests)
    add_test(NAME foundation.task-system-failure-injection COMMAND hs_task_system_failure_tests engine)
    add_test(NAME foundation.task-system-stop-reentry COMMAND hs_task_system_failure_tests reentry)
    add_test(NAME foundation.taskflow-admission-failure COMMAND hs_task_system_failure_tests backend)
    set_tests_properties(foundation.task-system-failure-injection foundation.task-system-stop-reentry
        foundation.taskflow-admission-failure PROPERTIES TIMEOUT 30)
    add_test(
        NAME architecture.layers
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_architecture.cmake
    )
    add_executable(hs_gameplay_internal_tests Tests/gameplay_internal_tests.cpp)
    target_link_libraries(hs_gameplay_internal_tests PRIVATE hs_gameplay)
    target_include_directories(hs_gameplay_internal_tests PRIVATE Source/Gameplay/Private)
    hs_configure_target(hs_gameplay_internal_tests)
    add_test(NAME simulation.internal-invariants COMMAND hs_gameplay_internal_tests)
    add_executable(hs_navigation_tests Tests/gameplay_navigation_tests.cpp)
    target_link_libraries(hs_navigation_tests PRIVATE hs_game_domain)
    target_include_directories(hs_navigation_tests PRIVATE Source/Gameplay/Private)
    hs_configure_target(hs_navigation_tests)
    add_test(NAME simulation.navigation COMMAND hs_navigation_tests)
    add_executable(hs_simulation_tests Tests/simulation_tests.cpp)
    target_link_libraries(hs_simulation_tests PRIVATE hs_core hs_game_rules hs_gameplay_development)
    hs_configure_target(hs_simulation_tests)
    add_test(NAME simulation.determinism-core COMMAND hs_simulation_tests)
    add_executable(hs_simulation_rules_serialization_tests
        Tests/simulation_rules_serialization_tests.cpp)
    target_link_libraries(hs_simulation_rules_serialization_tests PRIVATE hs_game_rules)
    hs_configure_target(hs_simulation_rules_serialization_tests)
    add_test(NAME simulation.canonical-rules
        COMMAND hs_simulation_rules_serialization_tests)
    if(HS_BUILD_RUNTIME OR HS_BUILD_RENDER_DATA_TESTS)
        add_executable(hs_render_graph_validation_tests
            Tests/render_graph_validation_tests.cpp
            Source/RendererD3D12/Private/render_graph_validation.cpp)
        target_include_directories(hs_render_graph_validation_tests PRIVATE
            Source/RendererD3D12/Private)
        target_link_libraries(hs_render_graph_validation_tests PRIVATE hs_core hs_render_data_values)
        target_compile_definitions(hs_render_graph_validation_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        hs_configure_target(hs_render_graph_validation_tests)
        add_test(NAME rendering.graph-range-validation
            COMMAND hs_render_graph_validation_tests)
        set_tests_properties(rendering.graph-range-validation PROPERTIES TIMEOUT 30)
    endif()
    if(HS_BUILD_CONTENT_TOOLS OR HS_BUILD_RENDER_DATA_TESTS)
    add_executable(hs_selected_generation_tests Tests/selected_generation_tests.cpp)
    target_link_libraries(hs_selected_generation_tests PRIVATE
        hs_asset_manifest nlohmann_json::nlohmann_json)
    hs_configure_target(hs_selected_generation_tests)
    add_test(NAME content.selected-generation COMMAND hs_selected_generation_tests)
    add_executable(hs_character_interchange_tests
        Tests/character_interchange_tests.cpp
        Source/Apps/Content/character_products.cpp
        Source/Apps/CharacterInterchange/character_interchange.cpp)
    target_link_libraries(hs_character_interchange_tests PRIVATE
        hs_render_data hs_geometry_cooker nlohmann_json::nlohmann_json)
    hs_configure_target(hs_character_interchange_tests)
    add_test(NAME content.character-interchange COMMAND hs_character_interchange_tests)
    add_executable(hs_render_package_tests
        Tests/render_package_tests.cpp
        Tests/shipping_image_identity_tests.cpp
        Tests/virtual_shadow_guarantee_tests.cpp
        Tests/release_audio_codegen_tests.cpp
        Source/Apps/Content/release_audio_codegen.cpp
        Source/Apps/RenderMaterialShader/material_graph_parser.cpp)
    target_include_directories(hs_render_package_tests PRIVATE Source/Apps/Content)
    target_link_libraries(hs_render_package_tests PRIVATE
        hs_material_compiler hs_render_data hs_runtime_contracts nlohmann_json::nlohmann_json)
    hs_configure_target(hs_render_package_tests)
    add_test(NAME content.render-package-format COMMAND hs_render_package_tests)
    if(HS_BUILD_CONTENT_TOOLS)
        add_executable(hs_scene_hlod_cooker_tests Tests/scene_hlod_cooker_tests.cpp
            Tests/asset_action_snapshot_tests.cpp)
        target_link_libraries(hs_scene_hlod_cooker_tests PRIVATE hs_scene_hlod_cooker hs_environment_layout
            hs_asset_pipeline nlohmann_json::nlohmann_json)
        hs_configure_target(hs_scene_hlod_cooker_tests)
        add_test(NAME content.scene-hlod-cooker COMMAND hs_scene_hlod_cooker_tests)
        add_executable(hs_environment_mesh_cooker_tests
            Tests/environment_mesh_cooker_tests.cpp
            Source/Apps/Content/environment_mesh_cooker.cpp)
        target_include_directories(hs_environment_mesh_cooker_tests PRIVATE Source/Apps/Content)
        target_link_libraries(hs_environment_mesh_cooker_tests PRIVATE hs_core nlohmann_json::nlohmann_json)
        hs_configure_target(hs_environment_mesh_cooker_tests)
        add_test(NAME content.environment-mesh-cooker COMMAND hs_environment_mesh_cooker_tests)
        set_tests_properties(content.environment-mesh-cooker PROPERTIES TIMEOUT 30)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME content.environment-family-batch-action
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_environment_family_batch.py"
                --producer "$<TARGET_FILE:hs_environment>"
                --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
                --build-root "${CMAKE_CURRENT_BINARY_DIR}/Artifacts/environment-family-batch"
            BUILD_DEPENDS hs_environment)
        set_tests_properties(content.environment-family-batch-action PROPERTIES TIMEOUT 1800)
        add_test(NAME content.material-parameter-actions
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_material_parameter_actions.py"
                --shader-exe "$<TARGET_FILE:hs_render_material_shader>"
                --package-exe "$<TARGET_FILE:hs_render_package>"
                --dxc "${HS_DXC}"
                --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
                --build-root "${CMAKE_CURRENT_BINARY_DIR}"
            BUILD_DEPENDS hs_render_material_shader hs_render_package hs_content_generation)
        set_tests_properties(content.material-parameter-actions PROPERTIES TIMEOUT 180)
        if(TARGET hs_static_shadow_bake)
            add_test(NAME content.static-shadow-action
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_static_shadow_action.py"
                    --producer "$<TARGET_FILE:hs_static_shadow_bake>"
                    --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
                    --build-root "${CMAKE_CURRENT_BINARY_DIR}"
                    --generation "${HS_STATIC_SHADOW_INPUT}"
                    --result-list "${HS_STATIC_SHADOW_INPUT_LIST}"
                BUILD_DEPENDS hs_render_static_shadow_candidate)
            set_tests_properties(content.static-shadow-action PROPERTIES TIMEOUT 90)
        endif()
        if(TARGET hs_render_scene_probe)
            add_test(NAME content.scene-probe-action
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_scene_probe_action.py"
                    --producer "$<TARGET_FILE:hs_render_scene_probe>"
                    --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
                    --build-root "${CMAKE_CURRENT_BINARY_DIR}"
                BUILD_DEPENDS hs_render_scene_probes)
            set_tests_properties(content.scene-probe-action PROPERTIES TIMEOUT 60)
        endif()
    endif()
    add_executable(hs_triangle_sample_tests Tests/triangle_sample_tests.cpp)
    target_link_libraries(hs_triangle_sample_tests PRIVATE hs_render_data)
    hs_configure_target(hs_triangle_sample_tests)
    add_test(NAME content.triangle-sample COMMAND hs_triangle_sample_tests)
    if(CMAKE_GENERATOR MATCHES "Ninja")
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME content.asset-action-restat
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_asset_action_restat.py"
                    --cmake "${CMAKE_COMMAND}" --ninja "${CMAKE_MAKE_PROGRAM}")
        endif()
    endif()
    endif()
    if(HS_BUILD_CONTENT_TOOLS)
    add_test(NAME content.audio-generation
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_audio_generation.ps1"
            -GenerationExecutable "$<TARGET_FILE:hs_generation>"
            -BuildRoot "${CMAKE_CURRENT_BINARY_DIR}"
            -SourceRoot "${CMAKE_CURRENT_SOURCE_DIR}"
        BUILD_DEPENDS hs_audio_generation)
    add_test(NAME content.generation-upstream-identity
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_generation_upstream_identity.ps1"
            -GenerationExecutable "$<TARGET_FILE:hs_generation>"
            -FixtureParent "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(hs_vfx_authoring_tests Tests/vfx_authoring_tests.cpp
        Tests/release_vfx_codegen_tests.cpp
        Tests/vfx_image_identity_tests.cpp
        Source/Apps/Content/release_vfx_codegen.cpp)
    target_link_libraries(hs_vfx_authoring_tests PRIVATE hs_vfx_authoring hs_vfx_image_io nlohmann_json::nlohmann_json)
    target_compile_definitions(hs_vfx_authoring_tests PRIVATE
        HS_RELEASE_VFX_CODEGEN_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/Artifacts/vfx-codegen")
    target_include_directories(hs_vfx_authoring_tests PRIVATE Source/Apps/Content)
    target_compile_definitions(hs_vfx_authoring_tests PRIVATE HS_VFX_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    hs_configure_target(hs_vfx_authoring_tests)
    add_dependencies(hs_vfx_authoring_tests hs_vfx_prepare)
    add_test(NAME content.vfx-v4-protect-source COMMAND hs_vfx_prepare --prepare "${CMAKE_CURRENT_SOURCE_DIR}/contentsource/vfx-invalid-output")
    set_tests_properties(content.vfx-v4-protect-source PROPERTIES WILL_FAIL TRUE)
    add_test(NAME content.vfx-v4-contract COMMAND hs_vfx_authoring_tests)
    set_tests_properties(content.vfx-v4-contract PROPERTIES FIXTURES_SETUP vfx-codegen)
    add_test(NAME content.vfx-release-codegen-compiled
        COMMAND ${CMAKE_COMMAND}
            "-DCXX=${CMAKE_CXX_COMPILER}" "-DCOMPILER_ID=${CMAKE_CXX_COMPILER_ID}"
            "-DCONFIG=$<CONFIG>" "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DFIXTURE_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/vfx-codegen"
            "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/vfx-codegen/compiled"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_release_vfx_codegen.cmake"
        BUILD_DEPENDS hs_vfx_authoring_tests)
    set_tests_properties(content.vfx-release-codegen-compiled PROPERTIES
        FIXTURES_REQUIRED vfx-codegen TIMEOUT 90)
    add_test(NAME tools.content-status COMMAND hs_generation --verify
        --build-root "${CMAKE_CURRENT_BINARY_DIR}" --scope runtime-full
        BUILD_DEPENDS hs_content_generation)
    endif()
    if(HS_BUILD_RUNTIME)
    set(HS_RENDER_TEST_VALIDATION_ARGS --validation)
    if(HS_ENABLE_GPU_VALIDATION)
        list(APPEND HS_RENDER_TEST_VALIDATION_ARGS --gpu-validation)
    endif()
    add_test(
        NAME integration.render-smoke
        COMMAND
            powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_render_baseline.ps1"
            -Executable "$<TARGET_FILE:ProjectHS>"
            -GenerationPointer "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/runtime-full.json"
            -ArtifactsRoot "${CMAKE_CURRENT_BINARY_DIR}/Artifacts/render-baseline"
    )
    set_tests_properties(integration.render-smoke PROPERTIES TIMEOUT 120)
    add_test(
        NAME vfx.showcase
        COMMAND
            ProjectHS --smoke --vfx-showcase --validation --ticks=60 --barriers=legacy
            --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/runtime-full.json
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
            -GenerationPointer "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/runtime-full.json"
            -BaselineRoot "${CMAKE_CURRENT_BINARY_DIR}/Artifacts/render-baseline"
    )
    set_tests_properties(rendering.barriers PROPERTIES TIMEOUT 120)
    add_test(
        NAME rendering.render-only
        COMMAND
            hs_render_test_runner
            --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-render.json
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/render-only
            --frames=60
            --barriers=enhanced
            ${HS_RENDER_TEST_VALIDATION_ARGS}
            --persistent-scene-probe
            --require-multi-queue
        BUILD_DEPENDS hs_render_test_runner hs_test_render_generation
    )
    set_tests_properties(rendering.render-only PROPERTIES TIMEOUT 120)
    add_test(NAME rendering.cluster-retest
        COMMAND hs_render_test_runner
            --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-render.json
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/cluster-retest
            --frames=14 --surface=visibility --geometry=indexed
            --barriers=enhanced ${HS_RENDER_TEST_VALIDATION_ARGS} --persistent-scene-probe
        BUILD_DEPENDS hs_render_test_runner hs_test_render_generation)
    set_tests_properties(rendering.cluster-retest PROPERTIES TIMEOUT 120)
    add_test(NAME rendering.grass-history
        COMMAND hs_render_test_runner
            --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-render.json
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/grass-history
            --frames=16 --grass-history-probe=D --grass-cadence=144
            --surface=visibility --geometry=indexed --barriers=enhanced ${HS_RENDER_TEST_VALIDATION_ARGS}
        BUILD_DEPENDS hs_render_test_runner hs_test_render_generation)
    set_tests_properties(rendering.grass-history PROPERTIES TIMEOUT 180 RESOURCE_LOCK projecths_d3d12)
    add_test(NAME rendering.local-probe-revisions
        COMMAND hs_render_test_runner
            --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/runtime-full.json
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/local-probe-revisions
            --frames=96 --barriers=enhanced --validation --local-probe-revision-probe
        BUILD_DEPENDS hs_render_test_runner hs_content_generation)
    set_tests_properties(rendering.local-probe-revisions PROPERTIES TIMEOUT 180)
    add_test(NAME rendering.static-shadow-bake
        COMMAND hs_render_test_runner
            --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-render.json
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/static-shadow-bake
            --barriers=enhanced --validation --static-shadow-bake-probe
        BUILD_DEPENDS hs_render_test_runner hs_test_render_generation)
    add_test(NAME rendering.static-shadow-bake-gbv
        COMMAND hs_render_test_runner
            --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-render.json
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/static-shadow-bake-gbv
            --barriers=enhanced --gpu-validation --static-shadow-bake-probe --static-shadow-bake-only
        BUILD_DEPENDS hs_render_test_runner hs_test_render_generation)
    set_tests_properties(rendering.static-shadow-bake rendering.static-shadow-bake-gbv
        PROPERTIES TIMEOUT 180 RESOURCE_LOCK projecths_d3d12)
    add_test(
        NAME rendering.skin-cache-gpu
        COMMAND
            hs_render_test_runner
            --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-render.json
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/skin-cache-gpu
            --frames=3
            --barriers=enhanced
            --validation
            --skin-cache-monster-probe
        BUILD_DEPENDS hs_render_test_runner hs_test_render_generation
    )
    set_tests_properties(rendering.skin-cache-gpu PROPERTIES TIMEOUT 120)
    add_test(NAME rendering.dynamic-lod-skin-growth
        COMMAND hs_render_test_runner
            --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-render.json
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/dynamic-lod-skin-growth
            --frames=8 --barriers=enhanced --gpu-validation
            --dynamic-lod-skin-growth-probe
            --require-multi-queue
        BUILD_DEPENDS hs_render_test_runner hs_test_render_generation)
    # GBV first-use shader patch compilation is CPU work: a cold, valid frame
    # measured 281 seconds before 0.233 seconds of GPU work on this test path.
    # Keep validation/assertions intact and allow that bounded cold startup.
    set_tests_properties(rendering.dynamic-lod-skin-growth PROPERTIES TIMEOUT 600)
    add_test(NAME rendering.timing-stream
        COMMAND hs_render_test_runner
            --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-render.json
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/timing-stream
            --frames=8 --timing-stream-probe
            --surface=compact --geometry=indexed --barriers=enhanced --validation
        BUILD_DEPENDS hs_render_test_runner hs_test_render_generation)
    set_tests_properties(rendering.timing-stream PROPERTIES
        TIMEOUT 600 RESOURCE_LOCK projecths_d3d12)
    add_test(NAME rendering.residency-streaming-gpu
        COMMAND hs_render_test_runner
            --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-render.json
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/residency-streaming
            --frames=128 --barriers=enhanced --gpu-validation
            --residency-streaming-probe
            --require-multi-queue
        BUILD_DEPENDS hs_render_test_runner hs_test_render_generation)
    set_tests_properties(rendering.residency-streaming-gpu PROPERTIES TIMEOUT 600)
    if(Python3_Interpreter_FOUND)
        add_test(NAME rendering.light-list
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Tests/analyze_light_list.py"
                "${CMAKE_CURRENT_BINARY_DIR}/Artifacts/light-list"
                "--runner=$<TARGET_FILE:hs_render_test_runner>"
                "--generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-render.json"
            BUILD_DEPENDS hs_render_test_runner hs_test_render_generation)
        add_test(NAME rendering.lighting-auxiliaries
            COMMAND hs_render_test_runner
                --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-render.json
                --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/lighting-auxiliaries
                --validation --barriers=enhanced --lighting-auxiliary-probe
            BUILD_DEPENDS hs_render_test_runner hs_test_render_generation)
        set_tests_properties(rendering.light-list rendering.lighting-auxiliaries PROPERTIES
            TIMEOUT 180 RESOURCE_LOCK projecths_d3d12)
        add_test(NAME rendering.oit-overdraw
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Tests/analyze_oit_overdraw.py"
                "${CMAKE_CURRENT_BINARY_DIR}/Artifacts/oit-overdraw"
                "--runner=$<TARGET_FILE:hs_render_test_runner>"
                "--generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-render.json"
                --require-finite
            BUILD_DEPENDS hs_render_test_runner hs_test_render_generation)
        add_test(NAME rendering.vfx-camera-basis
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Tests/analyze_oit_overdraw.py"
                "${CMAKE_CURRENT_BINARY_DIR}/Artifacts/vfx-camera-basis"
                "--runner=$<TARGET_FILE:hs_render_test_runner>"
                "--generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-render.json"
                --camera-basis
            BUILD_DEPENDS hs_render_test_runner hs_test_render_generation)
        set_tests_properties(rendering.oit-overdraw rendering.vfx-camera-basis PROPERTIES
            TIMEOUT 180 RESOURCE_LOCK projecths_d3d12)
    endif()
    if(HS_BUILD_CONTENT_TOOLS)
        add_test(NAME rendering.material-parameter-reload
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Tests/make_material_parameter_reload_fixture.py"
                --source-pointer "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-render.json"
                --authoring-root "${CMAKE_CURRENT_SOURCE_DIR}"
                --material-producer "$<TARGET_FILE:hs_render_material_shader>"
                --generation-verifier "$<TARGET_FILE:hs_generation>"
                --runner "$<TARGET_FILE:hs_render_test_runner>"
                --artifacts "${CMAKE_CURRENT_BINARY_DIR}/Artifacts/material-parameter-reload"
                --geometry indexed --surface visibility --barriers enhanced
            BUILD_DEPENDS hs_render_test_runner hs_render_material_shader hs_generation hs_test_render_generation)
        # Cold GBV shader instrumentation also exceeded 300 seconds in adjacent GPU fixtures.
        set_tests_properties(rendering.material-parameter-reload PROPERTIES
            TIMEOUT 600 RESOURCE_LOCK projecths_d3d12)
    endif()
    add_test(
        NAME experiment.offscreen
        COMMAND
            hs_experiment
            --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/runtime-full.json
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/experiment
        BUILD_DEPENDS hs_content_generation
    )
    set_tests_properties(experiment.offscreen PROPERTIES TIMEOUT 120)
    # These tests create D3D12 devices. Keep GPU captures serialized while
    # independent CPU validation runs alongside them under parallel CTest.
    set_tests_properties(
        integration.render-smoke vfx.showcase rendering.barriers rendering.render-only rendering.cluster-retest rendering.local-probe-revisions
        rendering.skin-cache-gpu
        rendering.dynamic-lod-skin-growth
        rendering.residency-streaming-gpu
        experiment.offscreen
        PROPERTIES RESOURCE_LOCK projecths_d3d12)
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
    target_link_libraries(hs_gameplay_tests PRIVATE hs_core hs_gameplay_development hs_game_rules_io hs_presentation hs_presentation_io hs_asset_manifest hs_render_data)
    hs_configure_target(hs_gameplay_tests)
    add_test(NAME content.environment-assets COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_environment_assets.ps1"
        -GenerationPointer "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-environment.json"
        BUILD_DEPENDS hs_test_environment_generation)
    add_test(NAME simulation.content COMMAND hs_gameplay_tests content
        BUILD_DEPENDS hs_test_gameplay_generation)
    add_test(NAME simulation.combat COMMAND hs_gameplay_tests combat
        BUILD_DEPENDS hs_test_gameplay_generation)
    add_test(NAME simulation.progression COMMAND hs_gameplay_tests progression
        BUILD_DEPENDS hs_test_gameplay_generation)
    add_test(NAME simulation.presentation-contract COMMAND hs_gameplay_tests presentation
        BUILD_DEPENDS hs_test_gameplay_generation)
    add_test(NAME simulation.determinism COMMAND hs_gameplay_tests determinism
        BUILD_DEPENDS hs_test_gameplay_generation)
    set_tests_properties(
        simulation.content
        simulation.combat
        simulation.progression
        simulation.presentation-contract
        simulation.determinism
        PROPERTIES TIMEOUT 120 ENVIRONMENT
            "HS_TEST_GENERATION_POINTER=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/test-gameplay.json;HS_TEST_GENERATION_SCOPE=test-gameplay"
    )
    endif()
    if(TARGET hs_combat_sim)
    if(HS_BUILD_CONTENT_TOOLS)
        set(HS_COMBAT_TEST_GENERATION_TARGET hs_test_combat_generation)
        set(HS_COMBAT_TEST_GENERATION_SCOPE test-combat)
    else()
        set(HS_COMBAT_TEST_GENERATION_TARGET hs_content_generation)
        set(HS_COMBAT_TEST_GENERATION_SCOPE runtime-full)
    endif()
    add_test(
        NAME scenario.combat-simulator
        COMMAND hs_combat_sim
            --suite=${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data/combat_suite_smoke.json
            --output=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/combat-simulator
            --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/${HS_COMBAT_TEST_GENERATION_SCOPE}.json
        BUILD_DEPENDS ${HS_COMBAT_TEST_GENERATION_TARGET}
    )
    set_tests_properties(scenario.combat-simulator PROPERTIES TIMEOUT 30)
    add_test(
        NAME tools.combat-suite-input
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_combat_suite_input.ps1
            -Binary $<TARGET_FILE:hs_combat_sim>
            -SourceRoot ${CMAKE_CURRENT_SOURCE_DIR}
            -GenerationPointer ${CMAKE_CURRENT_BINARY_DIR}/GenerationState/${HS_COMBAT_TEST_GENERATION_SCOPE}.json
        BUILD_DEPENDS ${HS_COMBAT_TEST_GENERATION_TARGET}
    )
    set_tests_properties(tools.combat-suite-input PROPERTIES TIMEOUT 30)
    endif()

    if(HS_BUILD_RUNTIME)
    if(HS_BUILD_CONTENT_TOOLS)
        set(HS_TYPED_VFX_GENERATION_TARGET hs_test_vfx_generation)
        set(HS_TYPED_VFX_GENERATION_SCOPE test-vfx)
        set(HS_RUNTIME_TEST_GENERATION_TARGET hs_test_runtime_generation)
        set(HS_RUNTIME_TEST_GENERATION_SCOPE test-runtime)
    else()
        set(HS_TYPED_VFX_GENERATION_TARGET hs_content_generation)
        set(HS_TYPED_VFX_GENERATION_SCOPE runtime-full)
        set(HS_RUNTIME_TEST_GENERATION_TARGET hs_content_generation)
        set(HS_RUNTIME_TEST_GENERATION_SCOPE runtime-full)
    endif()
    add_executable(hs_vfx_synthetic_tests
        Tests/vfx_synthetic_tests.cpp
        Tests/vfx_typed_decal_commands_tests.cpp
        Tests/vfx_typed_distortion_commands_tests.cpp
        Tests/vfx_typed_light_commands_tests.cpp
        Tests/vfx_typed_mesh_commands_tests.cpp
        Tests/vfx_typed_entity_surface_commands_tests.cpp
        Tests/vfx_upgrade_dispatch_tests.cpp
        Tests/vfx_legacy_commands_tests.cpp)
    target_link_libraries(hs_vfx_synthetic_tests PRIVATE hs_runtime hs_vfx_image_io hs_gameplay_development)
    target_include_directories(hs_vfx_synthetic_tests PRIVATE
        Source/Runtime/Private Source/RendererD3D12/Public Source/Presentation/Public)
    hs_configure_target(hs_vfx_synthetic_tests)
    add_test(NAME runtime.vfx-typed-decal COMMAND hs_vfx_synthetic_tests decal)
    add_test(NAME runtime.vfx-typed-distortion COMMAND hs_vfx_synthetic_tests distortion)
    add_test(NAME runtime.vfx-typed-light COMMAND hs_vfx_synthetic_tests light)
    add_test(NAME runtime.vfx-typed-mesh COMMAND hs_vfx_synthetic_tests mesh)
    add_test(NAME runtime.vfx-typed-entity-surface COMMAND hs_vfx_synthetic_tests entity_surface)
    add_test(NAME runtime.vfx-upgrade-dispatch COMMAND hs_vfx_synthetic_tests upgrade_dispatch)
    add_test(NAME runtime.vfx-legacy-prepared COMMAND hs_vfx_synthetic_tests legacy)

    add_executable(hs_vfx_cooked_tests
        Tests/vfx_cooked_tests.cpp
        Tests/vfx_typed_flash_commands_tests.cpp
        Tests/vfx_typed_ballistic_commands_tests.cpp
        Tests/vfx_typed_mote_commands_tests.cpp
        Tests/vfx_typed_ground_commands_tests.cpp
        Tests/vfx_typed_fresnel_commands_tests.cpp
        Tests/vfx_typed_ribbon_commands_tests.cpp)
    target_link_libraries(hs_vfx_cooked_tests PRIVATE hs_runtime hs_asset_manifest hs_vfx_image_io)
    target_sources(hs_vfx_cooked_tests PRIVATE
        $<$<CONFIG:Release>:$<TARGET_OBJECTS:hs_runtime_vfx_io>>)
    target_include_directories(hs_vfx_cooked_tests PRIVATE Source/Runtime/Private Source/RendererD3D12/Public)
    hs_configure_target(hs_vfx_cooked_tests)
    add_test(NAME runtime.vfx-typed-flash COMMAND hs_vfx_cooked_tests flash
        BUILD_DEPENDS ${HS_TYPED_VFX_GENERATION_TARGET})
    add_test(NAME runtime.vfx-typed-ballistic COMMAND hs_vfx_cooked_tests ballistic
        BUILD_DEPENDS ${HS_TYPED_VFX_GENERATION_TARGET})
    add_test(NAME runtime.vfx-typed-mote COMMAND hs_vfx_cooked_tests mote
        BUILD_DEPENDS ${HS_TYPED_VFX_GENERATION_TARGET})
    add_test(NAME runtime.vfx-typed-ground COMMAND hs_vfx_cooked_tests ground
        BUILD_DEPENDS ${HS_TYPED_VFX_GENERATION_TARGET})
    add_test(NAME runtime.vfx-typed-fresnel COMMAND hs_vfx_cooked_tests fresnel
        BUILD_DEPENDS ${HS_TYPED_VFX_GENERATION_TARGET})
    add_test(NAME runtime.vfx-typed-ribbon COMMAND hs_vfx_cooked_tests ribbon
        BUILD_DEPENDS ${HS_TYPED_VFX_GENERATION_TARGET})
    add_executable(hs_vfx_persistent_tests
        Tests/vfx_persistent_state_tests.cpp
        Source/RendererD3D12/Private/vfx_persistent_state.cpp)
    target_link_libraries(hs_vfx_persistent_tests PRIVATE hs_core hs_render_data_values)
    target_include_directories(hs_vfx_persistent_tests PRIVATE
        Source/RendererD3D12/Public Source/RendererD3D12/Private)
    hs_configure_target(hs_vfx_persistent_tests)
    add_test(NAME rendering.vfx-persistent-lifetime COMMAND hs_vfx_persistent_tests)
    add_executable(hs_render_graph_tests Tests/render_graph_tests.cpp)
    target_link_libraries(hs_render_graph_tests PRIVATE hs_renderer_d3d12 hs_render_data D3D12MemoryAllocator d3d12 dxgi)
    target_include_directories(hs_render_graph_tests PRIVATE Source/RendererD3D12/Private)
    target_compile_definitions(hs_render_graph_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    hs_configure_target(hs_render_graph_tests)
    add_dependencies(hs_render_graph_tests hs_runtime_native_assets)
    add_test(NAME rendering.transient-graph COMMAND hs_render_graph_tests)
    set_tests_properties(rendering.transient-graph PROPERTIES TIMEOUT 45
        RESOURCE_LOCK projecths_d3d12)
    add_executable(hs_skin_cache_tests Tests/skin_cache_tests.cpp)
    target_link_libraries(hs_skin_cache_tests PRIVATE hs_renderer_d3d12)
    target_include_directories(hs_skin_cache_tests PRIVATE
        Source/RendererD3D12/Private)
    hs_configure_target(hs_skin_cache_tests)
    add_test(NAME rendering.skin-cache COMMAND hs_skin_cache_tests)
    add_executable(hs_residency_streaming_tests Tests/residency_streaming_tests.cpp)
    target_link_libraries(hs_residency_streaming_tests PRIVATE hs_renderer_d3d12 D3D12MemoryAllocator)
    target_compile_definitions(hs_residency_streaming_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(hs_residency_streaming_tests PRIVATE
        Source/RendererD3D12/Private)
    hs_configure_target(hs_residency_streaming_tests)
    add_test(NAME rendering.residency-streaming COMMAND hs_residency_streaming_tests)
    add_executable(hs_render_graph_multi_queue_tests
        Tests/render_graph_multi_queue_tests.cpp
        Tests/texture_streaming_native_tests.cpp)
    target_link_libraries(hs_render_graph_multi_queue_tests PRIVATE
        hs_renderer_d3d12 hs_render_data D3D12MemoryAllocator d3d12 dxgi)
    target_include_directories(hs_render_graph_multi_queue_tests PRIVATE
        Source/RendererD3D12/Private)
    target_compile_definitions(hs_render_graph_multi_queue_tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX HS_TEST_DXC_PATH="${HS_DXC}")
    hs_configure_target(hs_render_graph_multi_queue_tests)
    add_dependencies(hs_render_graph_multi_queue_tests hs_runtime_native_assets)
    add_test(NAME rendering.multi-queue-graph
        COMMAND hs_render_graph_multi_queue_tests)
    set_tests_properties(rendering.multi-queue-graph PROPERTIES TIMEOUT 90
        RESOURCE_LOCK projecths_d3d12)
    if(HS_BUILD_CONTENT_TOOLS)
        set(HS_BSDF_PROBE_SHADER "${CMAKE_CURRENT_BINARY_DIR}/Generated/TestShaders/bsdf_sampling.dxil")
        add_custom_command(OUTPUT "${HS_BSDF_PROBE_SHADER}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/Generated/TestShaders"
            COMMAND "${HS_DXC}" -nologo -WX -O3 -Ges -E main -T cs_6_0
                -I "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders"
                -Fo "${HS_BSDF_PROBE_SHADER}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/bsdf_sampling.hlsl"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/bsdf_sampling.hlsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/local_probe_sampling.hlsli"
                "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/probe_ibl.hlsli"
                ${HS_MATERIAL_SURFACE_LIBRARY} ${HS_DXC_TOOL_INPUTS}
            VERBATIM)
        add_executable(hs_bsdf_sampling_tests
            Tests/bsdf_sampling_tests.cpp
            "${HS_BSDF_PROBE_SHADER}")
        target_link_libraries(hs_bsdf_sampling_tests PRIVATE d3d12 dxgi)
        target_include_directories(hs_bsdf_sampling_tests BEFORE PRIVATE "${agility_SOURCE_DIR}/build/native/include")
        target_compile_definitions(hs_bsdf_sampling_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        hs_configure_target(hs_bsdf_sampling_tests)
        add_dependencies(hs_bsdf_sampling_tests hs_runtime_native_assets)
        add_test(NAME rendering.bsdf-sampling
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_bsdf_sampling.ps1"
                -Runner "$<TARGET_FILE:hs_bsdf_sampling_tests>"
                -GenerationPointer "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/runtime-full.json"
                -ProbeShader "${HS_BSDF_PROBE_SHADER}"
            BUILD_DEPENDS hs_bsdf_sampling_tests hs_content_generation)
        set_tests_properties(rendering.bsdf-sampling PROPERTIES TIMEOUT 90 RESOURCE_LOCK projecths_d3d12)
    endif()
    set_tests_properties(
        runtime.vfx-typed-flash runtime.vfx-typed-ballistic
        runtime.vfx-typed-mote runtime.vfx-typed-ground
        runtime.vfx-typed-fresnel runtime.vfx-typed-ribbon
        PROPERTIES ENVIRONMENT
            "HS_TEST_GENERATION_POINTER=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/${HS_TYPED_VFX_GENERATION_SCOPE}.json;HS_TEST_GENERATION_SCOPE=${HS_TYPED_VFX_GENERATION_SCOPE}")
    add_executable(hs_vfx_ribbon_state_tests
        Tests/vfx_ribbon_state_tests.cpp
        Source/RendererD3D12/Private/vfx_ribbon_state.cpp)
    target_link_libraries(hs_vfx_ribbon_state_tests PRIVATE hs_core hs_render_data_values)
    target_include_directories(hs_vfx_ribbon_state_tests PRIVATE
        Source/RendererD3D12/Public Source/RendererD3D12/Private)
    hs_configure_target(hs_vfx_ribbon_state_tests)
    add_test(NAME runtime.vfx-ribbon-state COMMAND hs_vfx_ribbon_state_tests)
    add_executable(hs_runtime_tests Tests/runtime_tests.cpp)
    target_sources(hs_runtime_tests PRIVATE
        $<$<CONFIG:Release>:$<TARGET_OBJECTS:hs_runtime_audio_io>>
        $<$<CONFIG:Release>:$<TARGET_OBJECTS:hs_runtime_vfx_io>>)
    target_link_libraries(hs_runtime_tests PRIVATE hs_runtime hs_asset_manifest hs_presentation hs_presentation_io hs_game_rules_io hs_vfx_image_io)
    target_compile_definitions(hs_runtime_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_dependencies(hs_runtime_tests hs_runtime_assets)
    target_include_directories(hs_runtime_tests PRIVATE
        Source/Runtime/Private
        Source/RendererD3D12/Public
    )
    hs_configure_target(hs_runtime_tests)
    add_test(NAME runtime.integration COMMAND hs_runtime_tests
        BUILD_DEPENDS ${HS_RUNTIME_TEST_GENERATION_TARGET})
    set_tests_properties(runtime.integration PROPERTIES TIMEOUT 30 ENVIRONMENT
        "HS_TEST_GENERATION_POINTER=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/${HS_RUNTIME_TEST_GENERATION_SCOPE}.json;HS_TEST_GENERATION_SCOPE=${HS_RUNTIME_TEST_GENERATION_SCOPE}")

    if(HS_BUILD_CONTENT_TOOLS)
        set(HS_UI_TEXT_GENERATION_TARGET hs_test_gameplay_generation)
        set(HS_UI_TEXT_GENERATION_SCOPE test-gameplay)
    else()
        set(HS_UI_TEXT_GENERATION_TARGET ${HS_RUNTIME_TEST_GENERATION_TARGET})
        set(HS_UI_TEXT_GENERATION_SCOPE ${HS_RUNTIME_TEST_GENERATION_SCOPE})
    endif()
    if(TARGET hs_shipping_content)
        add_executable(hs_runtime_shipping_tests
            Tests/shipping_content_tests.cpp
            Tests/shipping_image_tests.cpp)
        target_sources(hs_runtime_shipping_tests PRIVATE
            $<$<CONFIG:Release>:$<TARGET_OBJECTS:hs_runtime_audio_io>>
            $<$<CONFIG:Release>:$<TARGET_OBJECTS:hs_runtime_vfx_io>>)
        target_link_libraries(hs_runtime_shipping_tests PRIVATE hs_runtime hs_asset_manifest hs_presentation
            hs_presentation_io hs_game_rules_io hs_shipping_content hs_vfx_image_io)
        target_compile_definitions(hs_runtime_shipping_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        add_dependencies(hs_runtime_shipping_tests hs_runtime_assets)
        target_include_directories(hs_runtime_shipping_tests PRIVATE
            Source/Runtime/Private
            Source/RendererD3D12/Public
        )
        hs_configure_target(hs_runtime_shipping_tests)
        add_test(NAME runtime.shipping-content COMMAND hs_runtime_shipping_tests
            BUILD_DEPENDS ${HS_RUNTIME_TEST_GENERATION_TARGET})
        set_tests_properties(runtime.shipping-content PROPERTIES TIMEOUT 30 ENVIRONMENT
            "HS_TEST_GENERATION_POINTER=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/${HS_RUNTIME_TEST_GENERATION_SCOPE}.json;HS_TEST_GENERATION_SCOPE=${HS_RUNTIME_TEST_GENERATION_SCOPE}")
        add_test(NAME runtime.ui-text-fit COMMAND hs_runtime_shipping_tests ui-text-fit
            BUILD_DEPENDS ${HS_UI_TEXT_GENERATION_TARGET})
        set_tests_properties(runtime.ui-text-fit PROPERTIES TIMEOUT 30 ENVIRONMENT
            "HS_TEST_GENERATION_POINTER=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/${HS_UI_TEXT_GENERATION_SCOPE}.json;HS_TEST_GENERATION_SCOPE=${HS_UI_TEXT_GENERATION_SCOPE}")
    endif()
    add_executable(hs_graphics_settings_store_tests
        Tests/graphics_settings_store_tests.cpp)
    target_link_libraries(hs_graphics_settings_store_tests PRIVATE hs_runtime)
    target_include_directories(hs_graphics_settings_store_tests PRIVATE Source/RendererD3D12/Public)
    hs_configure_target(hs_graphics_settings_store_tests)
    add_test(NAME runtime.graphics-settings-store
        COMMAND hs_graphics_settings_store_tests)
    add_executable(hs_runtime_native_input_tests Tests/runtime_native_input_tests.cpp)
    target_link_libraries(hs_runtime_native_input_tests PRIVATE hs_core)
    target_include_directories(hs_runtime_native_input_tests PRIVATE Source/Runtime/Private)
    hs_configure_target(hs_runtime_native_input_tests)
    add_test(NAME runtime.native-input COMMAND hs_runtime_native_input_tests)
    set_tests_properties(runtime.native-input PROPERTIES TIMEOUT 10)
    add_executable(hs_generation_reload_watch_tests Tests/generation_reload_watch_tests.cpp)
    target_include_directories(hs_generation_reload_watch_tests PRIVATE Source/Runtime/Private)
    hs_configure_target(hs_generation_reload_watch_tests)
    add_test(NAME runtime.generation-reload-watch COMMAND hs_generation_reload_watch_tests)
    if(HS_BUILD_CONTENT_TOOLS)
        add_executable(hs_audio_generation_consumer_tests
            Tests/audio_generation_consumer_tests.cpp)
        target_sources(hs_audio_generation_consumer_tests PRIVATE
            $<$<CONFIG:Release>:$<TARGET_OBJECTS:hs_runtime_audio_io>>)
        target_link_libraries(hs_audio_generation_consumer_tests PRIVATE
            hs_runtime hs_asset_manifest)
        hs_configure_target(hs_audio_generation_consumer_tests)
        add_test(NAME content.audio-consumer
            COMMAND hs_audio_generation_consumer_tests
                "${CMAKE_CURRENT_BINARY_DIR}"
            BUILD_DEPENDS hs_audio_generation)
        set_tests_properties(content.audio-consumer PROPERTIES TIMEOUT 30)
        if(CMAKE_GENERATOR MATCHES "Ninja")
            find_package(Python3 QUIET COMPONENTS Interpreter)
            if(Python3_Interpreter_FOUND)


                add_test(NAME content.audio-generation-lifetime
                    COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_audio_generation_lifetime.py"
                        --generation "$<TARGET_FILE:hs_generation>"
                        --audio "$<TARGET_FILE:hs_audio_content>"
                        --audio-probe "$<TARGET_FILE:hs_audio_generation_consumer_tests>"
                    BUILD_DEPENDS hs_generation hs_audio_content hs_audio_generation_consumer_tests)
                set_tests_properties(content.audio-generation-lifetime PROPERTIES TIMEOUT 30)
            endif()
        endif()
    endif()
    add_test(
        NAME experiment.simulation
        COMMAND
            hs_experiment --simulation-only
            --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/${HS_COMBAT_TEST_GENERATION_SCOPE}.json
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/experiment-simulation
        BUILD_DEPENDS ${HS_COMBAT_TEST_GENERATION_TARGET}
    )
    set_tests_properties(experiment.simulation PROPERTIES TIMEOUT 30)
    add_test(
        NAME experiment.timeline
        COMMAND
            hs_experiment --simulation-only --timeline-self-test
            --generation=${CMAKE_CURRENT_BINARY_DIR}/GenerationState/${HS_COMBAT_TEST_GENERATION_SCOPE}.json
            --artifacts=${CMAKE_CURRENT_BINARY_DIR}/Artifacts/experiment-timeline
        BUILD_DEPENDS ${HS_COMBAT_TEST_GENERATION_TARGET}
    )
    set_tests_properties(experiment.timeline PROPERTIES TIMEOUT 30)
    get_property(HS_SHADER_HOT_RELOAD_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(HS_SHADER_HOT_RELOAD_MULTI_CONFIG OR NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        # Single-config CTest presets need no -C; only multi-config tests need
        # a runtime configuration filter to exclude Release.
        set(HS_SHADER_HOT_RELOAD_CONFIGURATIONS)
        if(HS_SHADER_HOT_RELOAD_MULTI_CONFIG)
            set(HS_SHADER_HOT_RELOAD_CONFIGURATIONS CONFIGURATIONS Debug RelWithDebInfo MinSizeRel)
        endif()
        add_test(
            NAME rendering.shader-hot-reload
            COMMAND
                powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/Tests/shader_hot_reload.ps1"
                -Executable "$<TARGET_FILE:ProjectHS>"
                -Python "${Python3_EXECUTABLE}"
                -FixtureScript "${CMAKE_CURRENT_SOURCE_DIR}/Tests/make_shader_reload_fixture.py"
                -GenerationVerifier "$<TARGET_FILE:hs_generation>"
                -Artifacts "${CMAKE_CURRENT_BINARY_DIR}/Artifacts/shader-hot-reload"
                -GenerationPointer "${CMAKE_CURRENT_BINARY_DIR}/GenerationState/runtime-full.json"
            ${HS_SHADER_HOT_RELOAD_CONFIGURATIONS}
            BUILD_DEPENDS hs_content_generation
        )
        set_tests_properties(rendering.shader-hot-reload PROPERTIES TIMEOUT 300
            RESOURCE_LOCK projecths_d3d12)
    endif()
    endif()

    get_property(HS_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
    foreach(test_name IN LISTS HS_REGISTERED_TESTS)
        get_property(test_labels TEST ${test_name} PROPERTY LABELS)
        if(test_name MATCHES "^(foundation\\.|architecture\\.|build\\.)")
            list(APPEND test_labels core)
        endif()
        if(test_name MATCHES "^(simulation\\.|scenario\\.)")
            list(APPEND test_labels gameplay)
        endif()
        if(test_name MATCHES "^(content\\.|tools\\.content-)")
            list(APPEND test_labels content)
        endif()
        if(test_name MATCHES "^(runtime\\.|integration\\.)")
            list(APPEND test_labels runtime)
        endif()
        if(test_name MATCHES "^(rendering\\.|integration\\.render-smoke$|vfx\\.showcase$)")
            list(APPEND test_labels render)
        endif()
        if(test_name MATCHES "^(experiment\\.|tools\\.combat-suite-input$)")
            list(APPEND test_labels experiment)
        endif()
        get_test_property(${test_name} RESOURCE_LOCK test_resource_lock)
        if(test_resource_lock STREQUAL "projecths_d3d12")
            list(APPEND test_labels gpu)
        else()
            list(APPEND test_labels cpu)
        endif()
        list(REMOVE_DUPLICATES test_labels)
        set_tests_properties(${test_name} PROPERTIES LABELS "${test_labels}")
    endforeach()
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
    FILES "${winpix_SOURCE_DIR}/bin/x64/WinPixEventRuntime.dll"
    DESTINATION .
    COMPONENT Runtime
)
install(
    FILES "${HS_FONT_DIRECTORY}/NotoSansKR.ttf"
    DESTINATION Fonts
    CONFIGURATIONS Debug RelWithDebInfo MinSizeRel
    COMPONENT Runtime
)
install(
    FILES "${HS_FONT_DIRECTORY}/NotoSansKR-OFL.txt"
    DESTINATION Licenses
    COMPONENT Runtime
)
set(HS_GENERATION_EXE "${CMAKE_CURRENT_BINARY_DIR}/hs_generation.exe")
configure_file(cmake/install_selected_generation.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/install_selected_generation.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/install_selected_generation.cmake"
    COMPONENT Runtime)
install(FILES ThirdPartyNotices.txt DESTINATION . COMPONENT Runtime)
install(SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_release_finish.cmake"
    CONFIGURATIONS Release COMPONENT Runtime)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME ProjectHS)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(
    CPACK_INSTALL_CMAKE_PROJECTS
    "${CMAKE_BINARY_DIR};${PROJECT_NAME};Runtime;/"
)
include(CPack)
endif()

# Play builds do not compile development hosts; explicit targets remain available.
if(NOT HS_BUILD_TESTS)
    foreach(hs_development_host IN ITEMS hs_experiment hs_render_test_runner hs_combat_sim)
        if(TARGET ${hs_development_host})
            set_property(TARGET ${hs_development_host} PROPERTY EXCLUDE_FROM_ALL TRUE)
        endif()
    endforeach()
endif()
