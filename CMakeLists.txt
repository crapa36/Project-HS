cmake_minimum_required(VERSION 4.4)

project(ProjectHS VERSION 0.2.0 LANGUAGES C CXX)

# Single runtime pack location, shared by linker and installer (not a cache option).
set(HS_SHIPPING_PACK_RELATIVE_PATH "Content/content.hsp")
set(HS_SHIPPING_SOURCE_RELATIVE_PATH "Actions/shipping/content.generated.cpp")
set(HS_RUNTIME_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")

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

option(HS_BUILD_TESTS "Build Project HS tests" OFF)
if(HS_BUILD_TESTS)
    cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2.9)
else()
    # A verification configure can previously have registered tests in this same tree.
    file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake")
endif()
option(HS_BUILD_RUNTIME "Build the D3D12 runtime and applications" ON)
option(HS_BUILD_CONTENT_TOOLS "Build FBX content cooker and content-dependent tests" ON)
option(HS_WITH_DIRECTSTORAGE "Build the pinned native DirectStorage 1.3.0 asset IO path" OFF)

if(CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "Project HS supports single-config generators only (use the Ninja presets in CMakePresets.json).")
endif()
string(TOUPPER "${CMAKE_BUILD_TYPE}" HS_BUILD_TYPE_UPPER)
if(HS_BUILD_TYPE_UPPER STREQUAL "RELEASE")
    set(HS_SINGLE_CONFIG_RELEASE ON)
endif()
if(HS_BUILD_RUNTIME AND NOT HS_BUILD_CONTENT_TOOLS AND HS_SINGLE_CONFIG_RELEASE)
    foreach(shipping_input IN ITEMS
            "${HS_SHIPPING_SOURCE_RELATIVE_PATH}" "bin/${HS_SHIPPING_PACK_RELATIVE_PATH}")
        if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/${shipping_input}")
            message(FATAL_ERROR
                "Prebuilt Release requires ${shipping_input} in ${CMAKE_CURRENT_BINARY_DIR}; build with HS_BUILD_CONTENT_TOOLS=ON to produce the shipping pair first.")
        endif()
    endforeach()
endif()
if(HS_BUILD_RUNTIME AND HS_SINGLE_CONFIG_RELEASE)
    install(SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_release_begin.cmake" COMPONENT Runtime)
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

if(HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42F6E95CAD6EC532FD372391373363B62A14AF6D771056DBFC86160E6DFFF7AA
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

if(HS_BUILD_CONTENT_TOOLS OR (HS_BUILD_RUNTIME AND NOT HS_SINGLE_CONFIG_RELEASE))
    set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        directxtex
        GIT_REPOSITORY https://github.com/microsoft/DirectXTex.git
        GIT_TAG 4feb3e11a020f35b796fc769a74216a555d4f5ef
    )
    FetchContent_MakeAvailable(directxtex)
    include(cmake/patches/directxtex-bc7-batch.cmake)
    hs_patch_directxtex_bc7("${directxtex_SOURCE_DIR}")
endif()

if(HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS)
    set(HS_FONT_DIRECTORY "${FETCHCONTENT_BASE_DIR}/notosanskr")
    if(HS_BUILD_RUNTIME AND NOT HS_SINGLE_CONFIG_RELEASE)
        # The development runner can open loose content without a shipping image.
        set(HS_FONT_DIRECTORY "${HS_RUNTIME_DIRECTORY}/Fonts")
    endif()
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

    if(NOT HS_SINGLE_CONFIG_RELEASE)
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
    endif()

    set(D3D12MA_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
    set(D3D12MA_AGILITY_SDK_DIRECTORY "${agility_SOURCE_DIR}" CACHE STRING "" FORCE)
    FetchContent_Declare(
        d3d12ma
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.git
        GIT_TAG 1d86c1130f61453634b1df85782e1fecfd59a525
    )
    FetchContent_MakeAvailable(d3d12ma)
endif()

# External projects keep their own layout. Project outputs below share the preset tree.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/symbols")

function(hs_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_23)
    if(target MATCHES "^hs_.*tests$" OR target STREQUAL "hs_generation_reader_probe")
        set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL TRUE
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    endif()
    if(MSVC)
        get_target_property(kind ${target} TYPE)
        if(kind STREQUAL "EXECUTABLE")
            # Agility SDK exports are read from the executable; no client links to it.
            target_link_options(${target} PRIVATE /NOIMPLIB /NOEXP
                "/ILK:${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}.dir/${target}.ilk")
        endif()
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
target_compile_definitions(hs_jobs PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    $<$<NOT:$<CONFIG:Release>>:HS_DEVELOPMENT_TOOLS=1>)
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

# Developer files decode here; shipping tables decode from bounded binary sections.
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
target_compile_definitions(hs_gameplay PRIVATE $<$<NOT:$<CONFIG:Release>>:HS_DEVELOPMENT_TOOLS=1>)
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

if(HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS)
add_library(hs_generation_lock STATIC EXCLUDE_FROM_ALL
    Source/AssetManifest/Private/generation_lock.cpp)
target_include_directories(hs_generation_lock PUBLIC Source/AssetManifest/Public)
hs_configure_target(hs_generation_lock)

add_library(hs_asset_manifest STATIC EXCLUDE_FROM_ALL
    Source/AssetManifest/Private/selected_generation.cpp)
target_include_directories(hs_asset_manifest PUBLIC Source/AssetManifest/Public)
target_link_libraries(hs_asset_manifest PUBLIC hs_core hs_generation_lock PRIVATE nlohmann_json::nlohmann_json)
hs_configure_target(hs_asset_manifest)
endif()

# Public value/contract headers are usable without the native Runtime backend.
add_library(hs_runtime_contracts INTERFACE)
target_include_directories(hs_runtime_contracts INTERFACE Source/Runtime/Public Source/RendererD3D12/Public)
target_link_libraries(hs_runtime_contracts INTERFACE hs_core hs_game_domain hs_render_data_values)

if(HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS)
add_library(hs_audio_image_io STATIC EXCLUDE_FROM_ALL Source/Runtime/Private/audio_image_io.cpp)
target_link_libraries(hs_audio_image_io PUBLIC hs_runtime_contracts)
hs_configure_target(hs_audio_image_io)
endif()

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

if(HS_BUILD_RUNTIME OR HS_BUILD_CONTENT_TOOLS)
add_library(hs_meshoptimizer_codec STATIC ThirdParty/meshoptimizer/src/vertexcodec.cpp)
target_include_directories(hs_meshoptimizer_codec PUBLIC ThirdParty/meshoptimizer/src)
hs_configure_target(hs_meshoptimizer_codec)
if(MSVC)
    set_property(TARGET hs_meshoptimizer_codec PROPERTY MSVC_RUNTIME_CHECKS "")
    target_compile_options(hs_meshoptimizer_codec PRIVATE "$<$<CONFIG:Debug>:/O2;/Ob2>")
endif()
add_library(hs_render_data_package STATIC Source/RenderData/Private/render_asset_format.cpp)
target_link_libraries(hs_render_data_package PUBLIC hs_core hs_render_data_values)
hs_configure_target(hs_render_data_package)
add_library(hs_render_data_material STATIC
    Source/RenderData/Private/material_schema.cpp
    Source/RenderData/Private/material_sidecar.cpp)
target_link_libraries(hs_render_data_material PUBLIC hs_render_data_package)
hs_configure_target(hs_render_data_material)
add_library(hs_render_data_texture STATIC
    Source/RenderData/Private/texture_package.cpp
    Source/RenderData/Private/volume_texture_package.cpp)
target_link_libraries(hs_render_data_texture PUBLIC hs_render_data_package)
hs_configure_target(hs_render_data_texture)
add_library(hs_render_data STATIC
    Source/RenderData/Private/material_descriptor.cpp
    Source/RenderData/Private/render_revision.cpp
    Source/RenderData/Private/geometry_hierarchy.cpp
    Source/RenderData/Private/scene_hlod.cpp
    Source/RenderData/Private/scene_probe.cpp
    Source/RenderData/Private/static_shadow.cpp
    Source/RenderData/Private/static_shadow_archive.cpp
    Source/RenderData/Private/volumetric_medium.cpp
    Source/RenderData/Private/quality_profiles.cpp
    Source/RenderData/Private/character_animation_extension.cpp
    Source/RenderData/Private/character_package.cpp
    Source/RenderData/Private/geometry_package.cpp
    Source/RenderData/Private/lighting_probe.cpp)
target_include_directories(hs_render_data PUBLIC Source/RenderData/Public)
target_link_libraries(hs_render_data PUBLIC hs_core hs_render_data_values
    hs_render_data_package hs_render_data_material hs_render_data_texture)
target_link_libraries(hs_render_data PRIVATE hs_meshoptimizer_codec)
hs_configure_target(hs_render_data)
add_library(hs_renderer_catalog_io STATIC EXCLUDE_FROM_ALL
    Source/RenderData/Private/renderer_catalog.cpp
    Source/RenderData/Private/character_descriptor.cpp)
target_link_libraries(hs_renderer_catalog_io PUBLIC hs_render_data)
hs_configure_target(hs_renderer_catalog_io)
add_library(hs_terrain_surface STATIC Source/RenderData/Private/terrain_surface.cpp)
target_link_libraries(hs_terrain_surface PUBLIC hs_render_data)
hs_configure_target(hs_terrain_surface)
add_library(hs_probe_grid_bake STATIC EXCLUDE_FROM_ALL Source/RenderData/Private/scene_probe_grid_bake.cpp)
target_link_libraries(hs_probe_grid_bake PUBLIC hs_render_data)
hs_configure_target(hs_probe_grid_bake)
endif()

if(HS_BUILD_CONTENT_TOOLS)
add_library(hs_material_compiler STATIC Source/RenderData/Private/material_ir.cpp)
target_link_libraries(hs_material_compiler PUBLIC hs_render_data_material)
hs_configure_target(hs_material_compiler)

endif()

if(HS_BUILD_CONTENT_TOOLS)
add_library(hs_asset_pipeline STATIC Source/Apps/AssetPipeline/action.cpp)
target_link_libraries(hs_asset_pipeline PRIVATE hs_core hs_generation_lock nlohmann_json::nlohmann_json)
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

if(HS_BUILD_CONTENT_TOOLS)
add_executable(hs_generation Source/Apps/AssetGeneration/main.cpp)
target_link_libraries(hs_generation PRIVATE hs_core hs_generation_lock nlohmann_json::nlohmann_json)
target_compile_definitions(hs_generation PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_generation)
if(MSVC)
    target_sources(hs_generation PRIVATE "Source/Apps/AssetGeneration/long_paths.manifest")
endif()
endif()

if(HS_BUILD_RUNTIME)
add_library(
    hs_renderer_d3d12_common STATIC
    Source/RendererD3D12/Private/render_graph.cpp
    Source/RendererD3D12/Private/render_graph_validation.cpp
    Source/RendererD3D12/Private/persistent_gpu_scene.cpp
    Source/RendererD3D12/Private/residency_manager.cpp
    Source/RendererD3D12/Private/streaming_manager.cpp
    Source/RendererD3D12/Private/skin_cache.cpp
    Source/RendererD3D12/Private/renderer_diagnostics.cpp
    Source/RendererD3D12/Private/renderer_device.cpp
    Source/RendererD3D12/Private/vfx_ribbon_state.cpp
    Source/RendererD3D12/Private/renderer_pipeline.cpp
    Source/RendererD3D12/Private/renderer_pipeline_cache.cpp
    Source/RendererD3D12/Private/renderer_resources.cpp
    Source/RendererD3D12/Private/renderer_volume_texture.cpp
    Source/RendererD3D12/Private/renderer_resident_assets.cpp
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
    Source/RendererD3D12/Private/renderer_ambient_occlusion.cpp
    Source/RendererD3D12/Private/renderer_adaptive_shading.cpp
    Source/RendererD3D12/Private/renderer_screen_overlay.cpp
    Source/RendererD3D12/Private/renderer_lighting.cpp
    Source/RendererD3D12/Private/renderer_subsurface.cpp
    Source/RendererD3D12/Private/renderer_reflections.cpp
    Source/RendererD3D12/Private/renderer_transparency_targets.cpp
    Source/RendererD3D12/Private/renderer_translucency_diagnostics.cpp
    Source/RendererD3D12/Private/renderer_probe.cpp
    Source/RendererD3D12/Private/renderer_hdr_post.cpp
    Source/RendererD3D12/Private/renderer_entity_surface.cpp
    Source/RendererD3D12/Private/renderer_decal_surface.cpp
    Source/RendererD3D12/Private/renderer_decal_materials.cpp
    Source/RendererD3D12/Private/renderer_views.cpp
    Source/RendererD3D12/Private/renderer_settings.cpp
    Source/RendererD3D12/Private/renderer_named_animation.cpp
    Source/RendererD3D12/Private/renderer_virtual_shadow.cpp
    Source/RendererD3D12/Private/renderer_local_shadow.cpp
    Source/RendererD3D12/Private/renderer_hlod.cpp
    Source/RendererD3D12/Private/renderer_hlod_runtime.cpp
    Source/RendererD3D12/Private/renderer_volumetrics.cpp
    Source/RendererD3D12/Private/renderer_feature_resolve.cpp
    Source/RendererD3D12/Private/renderer_light_list.cpp
    Source/RendererD3D12/Private/renderer_character_material.cpp
    Source/RendererD3D12/Private/renderer_environment_mesh.cpp
    Source/RendererD3D12/Private/renderer_geometry_upload.cpp
    Source/RendererD3D12/Private/renderer_geometry.cpp
    Source/RendererD3D12/Private/renderer_procedural_geometry.cpp
    Source/RendererD3D12/Private/renderer_visibility_resources.cpp
    Source/RendererD3D12/Private/renderer_ui.cpp
)
target_include_directories(hs_renderer_d3d12_common PUBLIC Source/RendererD3D12/Public)
target_include_directories(
    hs_renderer_d3d12_common
    BEFORE PRIVATE "${agility_SOURCE_DIR}/build/native/include"
)
target_link_libraries(
    hs_renderer_d3d12_common
    PUBLIC hs_core hs_render_data hs_render_data_values hs_terrain_surface
    PRIVATE hs_renderer_catalog_io hs_jobs D3D12MemoryAllocator $<$<NOT:$<CONFIG:Release>>:WinPixEventRuntime> $<$<NOT:$<CONFIG:Release>>:DirectXTex> d3d12 dxgi dxguid windowscodecs gdi32
)
target_compile_definitions(hs_renderer_d3d12_common PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(
    hs_renderer_d3d12_common PUBLIC $<$<NOT:$<CONFIG:Release>>:HS_DEVELOPMENT_TOOLS=1>
)
target_compile_definitions(hs_renderer_d3d12_common PRIVATE $<$<NOT:$<CONFIG:Release>>:USE_PIX_RETAIL=1>)
hs_configure_target(hs_renderer_d3d12_common)

# Development renderer adapters stay out of the offline baker composition.
# The common native raster, residency, material and graph code is compiled once.
set(hs_renderer_composition
    Source/RendererD3D12/Private/renderer.cpp
    Source/RendererD3D12/Private/renderer_local_probe.cpp
    Source/RendererD3D12/Private/renderer_local_probe_runtime.cpp)
add_library(hs_renderer_d3d12 STATIC ${hs_renderer_composition}
    Source/RendererD3D12/Private/renderer_frame.cpp
    Source/RendererD3D12/Private/renderer_texture_feedback.cpp
    Source/RendererD3D12/Private/render_features.cpp
    Source/RendererD3D12/Private/renderer_post_subgraphs.cpp
    Source/RendererD3D12/Private/renderer_cluster_graph.cpp
    Source/RendererD3D12/Private/renderer_visibility.cpp
    Source/RendererD3D12/Private/renderer_lod.cpp
    Source/RendererD3D12/Private/renderer_transparency.cpp
    Source/RendererD3D12/Private/renderer_terrain_pages.cpp)
if(NOT HS_SINGLE_CONFIG_RELEASE)
    target_sources(
        hs_renderer_d3d12 PRIVATE
        Source/RendererD3D12/Private/renderer_tool_views.cpp
        Source/RendererD3D12/Private/renderer_capture.cpp
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp"
    )
    target_include_directories(
        hs_renderer_d3d12 PRIVATE "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends"
    )
endif()
add_library(hs_probe_baker STATIC EXCLUDE_FROM_ALL ${hs_renderer_composition}
    Source/RendererD3D12/Private/renderer_terrain_bake.cpp)
target_compile_definitions(hs_probe_baker PUBLIC HS_OFFLINE_PROBE_BAKER=1)
target_link_libraries(hs_probe_baker PRIVATE hs_probe_grid_bake)
foreach(adapter IN ITEMS hs_renderer_d3d12 hs_probe_baker)
    target_link_libraries(${adapter} PUBLIC hs_renderer_d3d12_common)
    target_include_directories(${adapter} BEFORE PRIVATE "$<TARGET_PROPERTY:hs_renderer_d3d12_common,INCLUDE_DIRECTORIES>")
    target_compile_definitions(${adapter} PRIVATE "$<TARGET_PROPERTY:hs_renderer_d3d12_common,COMPILE_DEFINITIONS>")
    hs_configure_target(${adapter})
endforeach()
if(MSVC)
    # Keep /Od, assertions and checked iterators; inline small STL/math calls in
    # the CPU frame path instead of stalling GPU submission behind Debug calls.
    set_property(SOURCE
        Source/RendererD3D12/Private/renderer_frame.cpp
        Source/RendererD3D12/Private/render_graph.cpp
        Source/RendererD3D12/Private/render_graph_validation.cpp
        APPEND PROPERTY COMPILE_OPTIONS "$<$<CONFIG:Debug>:/Ob2>")
endif()

add_library(hs_shipping_image_io STATIC EXCLUDE_FROM_ALL
    Source/Runtime/Private/shipping_content.cpp)
target_link_libraries(hs_shipping_image_io PUBLIC hs_runtime_contracts)
hs_configure_target(hs_shipping_image_io)

add_library(
    hs_runtime STATIC
    Source/Runtime/Private/application.cpp
    Source/Runtime/Private/renderer_settings_projection.cpp
    Source/Runtime/Private/prepared_content.cpp
    Source/Runtime/Private/audio_engine.cpp
    Source/Runtime/Private/save_store.cpp
    Source/Runtime/Private/vfx_catalog.cpp
    Source/Runtime/Private/vfx_typed_frame_adapter.cpp
    Source/Runtime/Private/vfx_persistent_state.cpp
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
    $<$<NOT:$<CONFIG:Release>>:Source/Runtime/Private/asset_test_room.hpp>
    $<$<NOT:$<CONFIG:Release>>:Source/Runtime/Private/asset_test_room.cpp>
    $<$<NOT:$<CONFIG:Release>>:Source/Runtime/Private/experiment_spec.cpp>
    $<$<NOT:$<CONFIG:Release>>:Source/Runtime/Private/playtest_recording.cpp>
)
target_link_libraries(
    hs_runtime
    PUBLIC hs_runtime_contracts hs_gameplay hs_render_data
    PRIVATE hs_shipping_image_io hs_jobs hs_presentation hs_renderer_d3d12 $<$<NOT:$<CONFIG:Release>>:hs_asset_manifest> nlohmann_json::nlohmann_json
            user32 shell32 xaudio2 ole32
)
target_compile_definitions(hs_runtime PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_definitions(
    hs_runtime PRIVATE $<$<NOT:$<CONFIG:Release>>:HS_DEVELOPMENT_TOOLS=1>
)
hs_configure_target(hs_runtime)
add_library(hs_runtime_audio_io OBJECT EXCLUDE_FROM_ALL
    Source/Runtime/Private/audio_engine_io.cpp)
target_link_libraries(hs_runtime_audio_io PRIVATE hs_runtime_contracts nlohmann_json::nlohmann_json)
target_compile_definitions(hs_runtime_audio_io PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
hs_configure_target(hs_runtime_audio_io)
target_sources(hs_runtime PRIVATE
    $<$<NOT:$<CONFIG:Release>>:$<TARGET_OBJECTS:hs_runtime_audio_io>>)
target_link_libraries(hs_runtime PRIVATE hs_vfx_image_io hs_audio_image_io hs_renderer_catalog_io hs_presentation_io hs_game_rules_io)
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
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/surface_layout.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/surface_metadata.hlsli")
set(HS_VISIBILITY_SHADER_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/visibility_buffer.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/visibility_classification.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/visibility_records.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/triangle_sample.hlsli")
set(HS_MATERIAL_SURFACE_LIBRARY
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/volumetrics_abi.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/local_probe_grid.hlsli"
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
    "${HS_RUNTIME_DIRECTORY}/D3D12/D3D12Core.dll")
if(NOT HS_SINGLE_CONFIG_RELEASE)
    hs_copy_runtime_asset(
        "${agility_SOURCE_DIR}/build/native/bin/x64/d3d12SDKLayers.dll"
        "${HS_RUNTIME_DIRECTORY}/D3D12/d3d12SDKLayers.dll")
    hs_copy_runtime_asset(
        "${winpix_SOURCE_DIR}/bin/x64/WinPixEventRuntime.dll"
        "${HS_RUNTIME_DIRECTORY}/WinPixEventRuntime.dll")
    set(HS_DEVELOPMENT_NATIVE_OUTPUTS
        "${HS_RUNTIME_DIRECTORY}/D3D12/d3d12SDKLayers.dll"
        "${HS_RUNTIME_DIRECTORY}/WinPixEventRuntime.dll")
endif()
include(cmake/RenderSDKs.cmake)
hs_configure_render_sdks()
add_custom_target(hs_runtime_native_assets DEPENDS
    "${HS_RUNTIME_DIRECTORY}/D3D12/D3D12Core.dll"
    ${HS_DEVELOPMENT_NATIVE_OUTPUTS}
    ${HS_RENDER_SDK_NATIVE_OUTPUTS})
add_custom_target(hs_runtime_assets DEPENDS ${HS_RUNTIME_ASSET_OUTPUTS})

add_executable(ProjectHS Source/Apps/ProjectHS/main.cpp
    Source/Apps/ProjectHS/command_line.cpp)
target_link_libraries(ProjectHS PRIVATE hs_runtime)
target_include_directories(ProjectHS BEFORE PRIVATE "${agility_SOURCE_DIR}/build/native/include")
target_compile_definitions(ProjectHS PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(ProjectHS PRIVATE $<$<NOT:$<CONFIG:Release>>:HS_DEVELOPMENT_TOOLS=1>)
hs_configure_target(ProjectHS)
if(MSVC)
    target_sources(ProjectHS PRIVATE "Source/Apps/Common/per_monitor_dpi.manifest")
endif()
add_dependencies(ProjectHS hs_runtime_assets)

if(NOT HS_SINGLE_CONFIG_RELEASE)
add_executable(hs_experiment Source/Apps/Experiment/main.cpp)
target_link_libraries(
    hs_experiment PRIVATE hs_runtime hs_gameplay hs_game_rules_io hs_presentation hs_asset_manifest nlohmann_json::nlohmann_json
)
target_compile_definitions(
    hs_experiment PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                          $<$<NOT:$<CONFIG:Release>>:HS_DEVELOPMENT_TOOLS=1>
)
hs_configure_target(hs_experiment)
if(MSVC)
    target_sources(hs_experiment PRIVATE "Source/Apps/Common/per_monitor_dpi.manifest")
endif()
add_dependencies(hs_experiment hs_runtime_assets)
endif()

if(HS_BUILD_TESTS)
add_executable(hs_render_test_runner
    Source/Apps/RenderTestRunner/main.cpp
    Source/Apps/RenderTestRunner/view_fixture.cpp
    Source/Apps/Common/world_transport_scene.cpp)
target_link_libraries(
    hs_render_test_runner PRIVATE hs_renderer_d3d12 hs_render_data hs_renderer_catalog_io hs_shipping_image_io hs_shipping_content hs_jobs hs_game_rules_io hs_environment_layout nlohmann_json::nlohmann_json
)
target_include_directories(hs_render_test_runner BEFORE PRIVATE "$<TARGET_PROPERTY:hs_renderer_d3d12_common,INCLUDE_DIRECTORIES>")
target_compile_definitions(hs_render_test_runner PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    "$<TARGET_PROPERTY:hs_renderer_d3d12_common,COMPILE_DEFINITIONS>")
if(MSVC)
    target_sources(hs_render_test_runner PRIVATE "Source/Apps/Common/per_monitor_dpi.manifest")
endif()
hs_configure_target(hs_render_test_runner)
add_dependencies(hs_render_test_runner hs_runtime_native_assets)
endif()
if(HS_BUILD_CONTENT_TOOLS)
    add_executable(hs_scene_probe_bake
        Source/Apps/SceneProbeBake/main.cpp
        Source/Apps/Common/world_transport_scene.cpp)
    target_link_libraries(hs_scene_probe_bake PRIVATE hs_asset_pipeline hs_probe_baker hs_render_data
        hs_asset_manifest hs_jobs hs_game_rules_io hs_environment_layout nlohmann_json::nlohmann_json)
    target_compile_definitions(hs_scene_probe_bake PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    hs_configure_target(hs_scene_probe_bake)
    if(MSVC)
        target_sources(hs_scene_probe_bake PRIVATE "Source/Apps/Common/per_monitor_dpi.manifest")
    endif()
    add_dependencies(hs_scene_probe_bake hs_runtime_native_assets)
    add_executable(hs_static_shadow_bake
        Source/Apps/StaticShadowBake/main.cpp
        Source/Apps/Common/world_transport_scene.cpp)
    target_link_libraries(hs_static_shadow_bake PRIVATE hs_asset_pipeline hs_probe_baker hs_render_data
        hs_asset_manifest hs_game_rules_io hs_environment_layout nlohmann_json::nlohmann_json)
    target_compile_definitions(hs_static_shadow_bake PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    hs_configure_target(hs_static_shadow_bake)
    if(MSVC)
        target_sources(hs_static_shadow_bake PRIVATE "Source/Apps/Common/per_monitor_dpi.manifest")
    endif()
    add_dependencies(hs_static_shadow_bake hs_runtime_native_assets)
    add_executable(hs_terrain_surface_bake
        Source/Apps/TerrainSurfaceBake/main.cpp
        Source/Apps/Common/world_transport_scene.cpp)
    target_link_libraries(hs_terrain_surface_bake PRIVATE hs_asset_pipeline hs_probe_baker hs_render_data hs_terrain_surface
        hs_asset_manifest hs_game_rules_io hs_environment_layout nlohmann_json::nlohmann_json DirectXTex d3d11 dxgi)
    target_compile_definitions(hs_terrain_surface_bake PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX)
    hs_configure_target(hs_terrain_surface_bake)
    if(MSVC)
        target_sources(hs_terrain_surface_bake PRIVATE "Source/Apps/Common/per_monitor_dpi.manifest")
    endif()
    add_dependencies(hs_terrain_surface_bake hs_runtime_native_assets)
endif()
endif()

if(HS_BUILD_CONTENT_TOOLS OR (HS_BUILD_RUNTIME AND NOT HS_SINGLE_CONFIG_RELEASE))
    add_executable(hs_combat_sim Source/Apps/CombatSim/main.cpp)
    target_link_libraries(
        hs_combat_sim PRIVATE hs_gameplay hs_game_rules_io hs_asset_manifest nlohmann_json::nlohmann_json
    )
    target_compile_definitions(hs_combat_sim PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    hs_configure_target(hs_combat_sim)
endif()

if(HS_BUILD_CONTENT_TOOLS)
add_library(hs_meshoptimizer_cook STATIC
    ThirdParty/meshoptimizer/src/indexgenerator.cpp
    ThirdParty/meshoptimizer/src/clusterizer.cpp
    ThirdParty/meshoptimizer/src/meshletutils.cpp
    ThirdParty/meshoptimizer/src/partition.cpp
    ThirdParty/meshoptimizer/src/simplifier.cpp
    ThirdParty/meshoptimizer/src/tangentspace.cpp)
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
        COMPILE_OPTIONS /fp:strict)
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
add_library(hs_content_json OBJECT Source/Apps/Content/content_json.cpp)
target_link_libraries(hs_content_json PRIVATE nlohmann_json::nlohmann_json)
hs_configure_target(hs_content_json)
include(cmake/GameData.cmake)
add_executable(hs_content_linker Source/Apps/ContentLinker/main.cpp)
target_compile_definitions(hs_content_linker PRIVATE
    HS_SHIPPING_PACK_RELATIVE_PATH="${HS_SHIPPING_PACK_RELATIVE_PATH}"
    HS_SHIPPING_SOURCE_RELATIVE_PATH="${HS_SHIPPING_SOURCE_RELATIVE_PATH}")
target_link_libraries(hs_content_linker PRIVATE hs_asset_pipeline hs_vfx_image_io
    hs_renderer_catalog_io nlohmann_json::nlohmann_json)
hs_configure_target(hs_content_linker)
add_executable(hs_environment
    Source/Apps/Environment/main.cpp
    Source/Apps/Content/environment_histogram_cooker.cpp
    Source/Apps/Content/environment_texture_cooker.cpp)
target_link_libraries(hs_environment PRIVATE hs_asset_pipeline)
target_include_directories(hs_environment PRIVATE Source/Apps/Content)
target_link_libraries(hs_environment PRIVATE
    hs_core nlohmann_json::nlohmann_json DirectXTex d3d11 dxgi ole32 bcrypt)
target_compile_definitions(hs_environment PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_environment)
add_executable(hs_environment_mesh_cook
    Source/Apps/EnvironmentMeshes/main.cpp
    Source/Apps/Content/environment_mesh_cooker.cpp)
target_link_libraries(hs_environment_mesh_cook PRIVATE hs_asset_pipeline hs_core nlohmann_json::nlohmann_json)
hs_configure_target(hs_environment_mesh_cook)
set(HS_ENVIRONMENT_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/Environment/project_hs_environment_materials_v9_LEAN.json")
hs_add_asset_action(
    NAME hs_environment_meshes
    BUILDER hs_environment_mesh_cook
    RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/meshes/result.json"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/meshes/result.d"
    STATIC_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Environment/environment_meshes.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Models/Environment/meadow_surface_meshes.json"
    ARGS --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --build-root "${CMAKE_CURRENT_BINARY_DIR}")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS hs_environment_texture=1)
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
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/histogram/result.json" ${HS_ENVIRONMENT_TEXTURE_RESULTS}
    JOB_POOL hs_environment_texture
    VERBATIM)
add_custom_target(hs_environment_products
    DEPENDS
        "${HS_ENVIRONMENT_TEXTURE_FAMILY_STAMP}"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/meshes/result.json")
add_executable(hs_lighting_probe
    Source/Apps/LightingProbe/main.cpp
    Source/Apps/LightingProbe/coated_dfg_cooker.cpp)
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
    ARGS --produce --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --build-root "${CMAKE_CURRENT_BINARY_DIR}")
set(HS_COATED_DFG_RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/coated-dfg/result.json")
hs_add_asset_action(
    BUILDER hs_lighting_probe
    RESULT "${HS_COATED_DFG_RESULT}"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/coated-dfg/result.d"
    ARGS --coated-dfg --build-root "${CMAKE_CURRENT_BINARY_DIR}")
add_executable(hs_render_texture_package
    Source/Apps/RenderTexturePackage/main.cpp)
target_link_libraries(hs_render_texture_package PRIVATE hs_asset_pipeline)
target_link_libraries(hs_render_texture_package PRIVATE
    hs_render_data_texture hs_core nlohmann_json::nlohmann_json bcrypt)
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
        BUILDER hs_render_texture_package
        RESULT "${texture_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/environment_${texture_name}/result.d"
        UPSTREAM_RESULTS "${texture_upstream}"
        ARGS --build-root "${CMAKE_CURRENT_BINARY_DIR}" --name "${texture_name}")
    list(APPEND HS_RENDER_TEXTURE_RESULTS "${texture_result}")
endforeach()
add_library(hs_material_graph_parser OBJECT Source/Apps/RenderMaterialShader/material_graph_parser.cpp)
target_link_libraries(hs_material_graph_parser PUBLIC hs_material_compiler PRIVATE nlohmann_json::nlohmann_json)
hs_configure_target(hs_material_graph_parser)
add_executable(hs_render_package Source/Apps/RenderPackage/main.cpp)
target_link_libraries(hs_render_package PRIVATE hs_asset_pipeline)
target_link_libraries(hs_render_package PRIVATE
    hs_geometry_cooker hs_material_graph_parser hs_render_data hs_core nlohmann_json::nlohmann_json bcrypt)
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
    Source/Apps/RenderMaterialShader/main.cpp)
target_link_libraries(hs_render_material_shader PRIVATE hs_asset_pipeline)
target_link_libraries(hs_render_material_shader PRIVATE
    hs_material_graph_parser hs_core nlohmann_json::nlohmann_json bcrypt)
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
foreach(family IN ITEMS terrain-page project particle-simulation fullscreen ui-draw geometry visibility visibility-classification static-visibility depth-pyramid skin-cache cluster-refinement ambient-occlusion adaptive-gtao reconstruction-guides sdk-presentation hdr-post light-list virtual-shadow volumetrics scene-hlod local-probe-bake)
    if(family STREQUAL "project" OR family STREQUAL "fullscreen")
        set(family_source project_hs.hlsl)
    elseif(family STREQUAL "visibility")
        set(family_source visibility_buffer.hlsl)
    else()
        string(REPLACE "-" "_" family_source "${family}")
        string(APPEND family_source ".hlsl")
    endif()
    set(family_extra_inputs)
    if(family STREQUAL "static-visibility" OR family STREQUAL "cluster-refinement")
        list(APPEND family_extra_inputs "${HS_HZB_REGION_SOURCE}")
    endif()
    if(family STREQUAL "sdk-presentation")
        list(APPEND family_extra_inputs "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/sdk_presentation_abi.hlsli")
    endif()
    set(family_result "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/runtime/${family}/result.json")
    hs_add_asset_action(
        BUILDER hs_render_runtime_shader
        RESULT "${family_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/runtime/${family}/result.d"
        STATIC_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/${family_source}" ${family_extra_inputs}
            ${HS_DXC_TOOL_INPUTS}
        ARGS "--family=${family}" "--source=${CMAKE_CURRENT_SOURCE_DIR}"
            "--build=${CMAKE_CURRENT_BINARY_DIR}" "--dxc=${HS_DXC}")
    list(APPEND HS_RENDER_SHADER_RESULTS "${family_result}")
endforeach()
set(HS_SURFACE_SHADER_RESULT
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/surface-passes/result.json")
set(HS_AUTHORED_MATERIAL_NAMES
    trunk_0 trunk_1 trunk_2 leaf_0 leaf_1 leaf_2 rock_0 rock_1 rock_2 rock_3
    grass_0 grass_1 grass_2 grass_3 terrain ground_detail)
set(HS_GROUND_DETAIL_MESH_NAMES
    flower_daisy flower_buttercup flower_violet clover_patch ground_leaves twig_cluster fence_short
    shrub_low stump_roots crate_intact crate_broken barrel_intact barrel_broken cart_broken fence_broken fence_splitrail sack_tied fallen_log rock_mossy)
set(HS_CHARACTER_MATERIAL_NAMES
    archer_skin archer_cloth archer_cutout archer_equipment slime_features slime_body monster_pbr)
set(HS_DEFERRED_MATERIAL_INPUTS)
foreach(name IN LISTS HS_AUTHORED_MATERIAL_NAMES HS_CHARACTER_MATERIAL_NAMES)
    list(APPEND HS_DEFERRED_MATERIAL_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/${name}.json")
endforeach()
hs_add_asset_action(
    NAME hs_render_surface_passes
    BUILDER hs_render_material_shader
    RESULT "${HS_SURFACE_SHADER_RESULT}"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/surface-passes/result.d"
    STATIC_INPUTS
        "${HS_SHADER_SOURCE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/layered_translucency.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/environment.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/environment_wind.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/grass_deformation.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_terrain.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/terrain_page_abi.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/terrain_histogram_abi.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/vfx_ribbon.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/deferred_surface.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/decal_surface_abi.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/subsurface_diffusion.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/vfx_screen_overlay.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/adaptive_shading.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/probe_ibl.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/light_list.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/light_list_abi.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/local_shadow.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/local_shadow_abi.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/shadow_data.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/temporal_reconstruction.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/display_transform.hlsli"
        ${HS_DEFERRED_MATERIAL_INPUTS}
        "${HS_GPU_SCENE_TABLES_SOURCE}" ${HS_GEOMETRY_SHADER_SOURCE}
        ${HS_MATERIAL_SURFACE_LIBRARY} ${HS_DXC_TOOL_INPUTS}
    ARGS --surface-passes --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --build-root "${CMAKE_CURRENT_BINARY_DIR}" --dxc "${HS_DXC}")
list(APPEND HS_RENDER_SHADER_RESULTS "${HS_SURFACE_SHADER_RESULT}")
set(HS_SSR_SHADER_RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/screen-reflections/result.json")
hs_add_asset_action(
    NAME hs_render_screen_reflections
    BUILDER hs_render_material_shader
    RESULT "${HS_SSR_SHADER_RESULT}"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/screen-reflections/result.d"
    STATIC_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/screen_reflections_abi.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/screen_reflections.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/screen_reflection_confidence.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/deferred_surface.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/decal_surface_abi.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/character_material.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_sidecar.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_parameters.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/surface_layout.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/surface_metadata.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/render_binding_contract.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_surface.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/bsdf_metallic_roughness.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/bsdf_layers.hlsli"
        ${HS_DEFERRED_MATERIAL_INPUTS} ${HS_DXC_TOOL_INPUTS}
    ARGS --screen-reflections --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --build-root "${CMAKE_CURRENT_BINARY_DIR}" --dxc "${HS_DXC}")
list(APPEND HS_RENDER_SHADER_RESULTS "${HS_SSR_SHADER_RESULT}")
# Intrusive developer PS variants are not part of Release/shipping products.
# Current content generations use the repository's single-config Ninja trees.
if(NOT HS_SINGLE_CONFIG_RELEASE)
    set(HS_TRANSLUCENCY_DIAGNOSTICS_SHADER_RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/translucency-diagnostics/result.json")
hs_add_asset_action(
    NAME hs_render_translucency_diagnostics
    BUILDER hs_render_material_shader
    RESULT "${HS_TRANSLUCENCY_DIAGNOSTICS_SHADER_RESULT}"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/translucency-diagnostics/result.d"
    STATIC_INPUTS
        "${HS_SHADER_SOURCE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/layered_translucency.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/environment.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/environment_wind.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/grass_deformation.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_terrain.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/terrain_page_abi.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/terrain_histogram_abi.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/vfx_ribbon.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/deferred_surface.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/decal_surface_abi.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/subsurface_diffusion.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/vfx_screen_overlay.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/adaptive_shading.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/probe_ibl.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/light_list.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/light_list_abi.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/local_shadow.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/local_shadow_abi.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/shadow_data.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/temporal_reconstruction.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/display_transform.hlsli"
        ${HS_DEFERRED_MATERIAL_INPUTS}
        "${HS_GPU_SCENE_TABLES_SOURCE}" ${HS_GEOMETRY_SHADER_SOURCE}
        ${HS_MATERIAL_SURFACE_LIBRARY} ${HS_DXC_TOOL_INPUTS}
    ARGS --translucency-diagnostics --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --build-root "${CMAKE_CURRENT_BINARY_DIR}" --dxc "${HS_DXC}")
endif()
set(HS_RENDER_ACTION_RESULTS)
foreach(name IN LISTS HS_AUTHORED_MATERIAL_NAMES)
    set(action_result "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/${name}/result.json")
    set(material_upstream "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/${name}/result.json")
    set(parameter_result "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/${name}/parameters/result.json")
    set(material_source "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/${name}.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${material_source}")
    file(READ "${material_source}" material_document)
    set(material_texture_results)
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
        BUILDER hs_render_material_shader
        RESULT "${material_upstream}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/${name}/result.d"
        STATIC_INPUTS
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/${name}.json"
            ${HS_DEFERRED_MATERIAL_INPUTS}
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/project_hs.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/environment.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/environment_wind.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/grass_deformation.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/local_probe_capture.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/terrain_page_abi.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/terrain_histogram_abi.hlsli"
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
        BUILDER hs_render_material_shader
        RESULT "${parameter_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/${name}/parameters/result.d"
        STATIC_INPUTS "${material_source}"
        ARGS --parameters --name "${name}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}")
    list(APPEND HS_RENDER_SHADER_RESULTS "${material_upstream}")
    list(APPEND HS_RENDER_ACTION_RESULTS "${action_result}" "${parameter_result}")
    hs_add_asset_action(
        BUILDER hs_render_package
        RESULT "${action_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/${name}/result.d"
        STATIC_INPUTS
            "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/${name}.json"
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
        BUILDER hs_render_package
        RESULT "${action_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/${name}/result.d"
        STATIC_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/${surface_material}.json"
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
set(HS_SCENE_HLOD_RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/hlod/arena/result.json")
set(HS_SCENE_HLOD_UPSTREAMS "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/simulation-payload/result.json")
set(HS_SCENE_RENDER_RESULTS
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/simulation-payload/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/terrain/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/terrain/parameters/result.json")
foreach(name IN LISTS HS_AUTHORED_MATERIAL_NAMES)
    if(name STREQUAL "terrain")
        continue()
    endif()
    list(APPEND HS_SCENE_RENDER_RESULTS
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/${name}/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/${name}/parameters/result.json")
    if(NOT name MATCHES "^grass_")
        list(APPEND HS_SCENE_HLOD_UPSTREAMS "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/${name}/result.json")
    endif()
endforeach()
list(APPEND HS_SCENE_HLOD_UPSTREAMS ${HS_HLOD_GROUND_DETAIL_GEOMETRY_RESULTS})
hs_add_asset_action(NAME hs_render_scene_hlod BUILDER hs_render_scene_package
    RESULT "${HS_SCENE_HLOD_RESULT}"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/hlod/arena/result.d"
    UPSTREAM_RESULTS ${HS_SCENE_HLOD_UPSTREAMS}
    ARGS --source-root "${CMAKE_CURRENT_SOURCE_DIR}" --build-root "${CMAKE_CURRENT_BINARY_DIR}")
set(HS_SCENE_PROBE_RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/local-probes/result.json")
list(APPEND HS_SCENE_RENDER_RESULTS
    ${HS_GROUND_DETAIL_GEOMETRY_RESULTS} ${HS_ENVIRONMENT_TEXTURE_RESULTS} ${HS_RENDER_TEXTURE_RESULTS}
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/meshes/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/histogram/result.json")
foreach(name IN LISTS HS_AUTHORED_MATERIAL_NAMES)
    list(APPEND HS_SCENE_RENDER_RESULTS
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/${name}/result.json")
endforeach()
list(REMOVE_DUPLICATES HS_SCENE_RENDER_RESULTS)
set(HS_SCENE_PROBE_RESOLVE_RESULTS "${HS_LIGHTING_PROBE_RESULT}" ${HS_SCENE_RENDER_RESULTS})
set(HS_SCENE_PROBE_RESULT_LIST "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/hs-content/scene-probe-input-results.txt")
hs_write_result_list("${HS_SCENE_PROBE_RESULT_LIST}" ${HS_SCENE_PROBE_RESOLVE_RESULTS})
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
foreach(name IN LISTS HS_CHARACTER_MATERIAL_NAMES)
    set(hero_result "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/${name}/result.json")
    set(hero_parameters "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/${name}/parameters/result.json")
    hs_add_asset_action(
        BUILDER hs_render_material_shader
        RESULT "${hero_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/${name}/result.d"
        STATIC_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/${name}.json"
        ARGS --name "${name}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}")
    hs_add_asset_action(
        BUILDER hs_render_material_shader
        RESULT "${hero_parameters}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/${name}/parameters/result.d"
        STATIC_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/${name}.json"
        ARGS --parameters --name "${name}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}")
    list(APPEND HS_RENDER_SHADER_RESULTS "${hero_result}")
    list(APPEND HS_RENDER_ACTION_RESULTS "${hero_parameters}")
endforeach()
add_custom_target(hs_shaders DEPENDS ${HS_RENDER_SHADER_RESULTS})
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
        BUILDER hs_render_material_shader
        RESULT "${decal_parameters}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/${name}/parameters/result.d"
        STATIC_INPUTS "${decal_source}"
        ARGS --parameters --name "${name}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()
hs_add_asset_action(
    NAME hs_render_material_shader_decal_surface BUILDER hs_render_material_shader
    RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/decal-surface/result.json"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/decal-surface/result.d"
    STATIC_INPUTS ${HS_DECAL_MATERIAL_SOURCES}
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/decal_surface.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/decal_surface_abi.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_surface.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_parameters.hlsli"
        ${HS_DXC_TOOL_INPUTS}
    ARGS --decal-surface --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
        --build-root "${CMAKE_CURRENT_BINARY_DIR}" --dxc "${HS_DXC}")
add_library(hs_character_geometry_cooker STATIC Source/Apps/Content/character_products.cpp)
target_link_libraries(hs_character_geometry_cooker PRIVATE
    hs_core hs_render_data hs_geometry_cooker hs_meshoptimizer_cook nlohmann_json::nlohmann_json)
target_compile_definitions(hs_character_geometry_cooker PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_character_geometry_cooker)
if(MSVC)
    set_property(TARGET hs_character_geometry_cooker PROPERTY MSVC_RUNTIME_CHECKS "")
    target_compile_options(hs_character_geometry_cooker PRIVATE "$<$<CONFIG:Debug>:/O2;/Ob2>")
endif()
add_executable(hs_character_content
    Source/Apps/CharacterContent/main.cpp
    Source/Apps/Content/character_asset_cooker.cpp
    Source/Apps/Content/character_fbx.cpp)
target_link_libraries(hs_character_content PRIVATE hs_asset_pipeline)
target_include_directories(hs_character_content PRIVATE Source/Apps/Content)
target_include_directories(hs_character_content SYSTEM PRIVATE "${HS_FBX_SDK_ROOT}/include")
target_link_libraries(hs_character_content PRIVATE
    hs_core hs_render_data hs_character_geometry_cooker hs_meshoptimizer_cook nlohmann_json::nlohmann_json
    fbxsdk fbx_xml fbx_zlib ws2_32 ole32 bcrypt)
target_compile_definitions(hs_character_content PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_character_content)
set(HS_CHARACTER_ACTION_RESULTS)
set(HS_CHARACTER_MODES archer slime-melee slime-ranged slime-suicide slime-projectile
    boss-5m boss-10m boss-final)
set(HS_CHARACTER_STEMS archer enemy_melee enemy_ranged enemy_suicide enemy_gel_projectile
    boss_5m boss_10m boss_final)
set(HS_HERO_BINDINGS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Materials/hero_bindings.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HS_HERO_BINDINGS_SOURCE}")
file(READ "${HS_HERO_BINDINGS_SOURCE}" HS_HERO_BINDINGS_JSON)
foreach(mode character_stem IN ZIP_LISTS HS_CHARACTER_MODES HS_CHARACTER_STEMS)
    set(character_material_results)
    string(JSON binding_count LENGTH "${HS_HERO_BINDINGS_JSON}" characters "${character_stem}")
    math(EXPR binding_last "${binding_count} - 1")
    foreach(binding_index RANGE 0 ${binding_last})
        string(JSON material_count ERROR_VARIABLE materials_error LENGTH
            "${HS_HERO_BINDINGS_JSON}" characters "${character_stem}" ${binding_index} materials)
        if(materials_error)
            string(JSON material_name GET "${HS_HERO_BINDINGS_JSON}" characters "${character_stem}" ${binding_index} material)
            list(APPEND character_material_results
                "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/${material_name}/result.json")
        else()
            math(EXPR material_last "${material_count} - 1")
            foreach(material_index RANGE 0 ${material_last})
                string(JSON material_name GET "${HS_HERO_BINDINGS_JSON}" characters "${character_stem}"
                    ${binding_index} materials ${material_index})
                list(APPEND character_material_results
                    "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/${material_name}/result.json")
            endforeach()
        endif()
    endforeach()
    list(REMOVE_DUPLICATES character_material_results)
    hs_add_asset_action(
        BUILDER hs_character_content
        RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/character/${mode}/result.json"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/character/${mode}/result.d"
        UPSTREAM_RESULTS ${character_material_results}
        STATIC_INPUTS "${HS_HERO_BINDINGS_SOURCE}"
        ARGS "--${mode}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}")
    list(APPEND HS_CHARACTER_ACTION_RESULTS
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/character/${mode}/result.json")
endforeach()
add_custom_target(hs_character_products DEPENDS ${HS_CHARACTER_ACTION_RESULTS})
add_executable(hs_render_character_texture_package
    Source/Apps/RenderCharacterTexturePackage/main.cpp
    Source/Apps/Content/character_fbx.cpp
    Source/Apps/Content/character_texture_cooker.cpp)
target_link_libraries(hs_render_character_texture_package PRIVATE hs_asset_pipeline)
target_include_directories(hs_render_character_texture_package SYSTEM PRIVATE "${HS_FBX_SDK_ROOT}/include")
target_link_libraries(hs_render_character_texture_package PRIVATE
    hs_render_data hs_core nlohmann_json::nlohmann_json
    DirectXTex fbxsdk fbx_xml fbx_zlib ws2_32 windowscodecs d3d11 ole32 bcrypt)
target_compile_definitions(hs_render_character_texture_package PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
hs_configure_target(hs_render_character_texture_package)
set(HS_CHARACTER_TEXTURE_RESULTS)
foreach(mode IN ITEMS ${HS_CHARACTER_MODES} monster-materials)
    set(texture_result
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/character/${mode}/result.json")
    hs_add_asset_action(
        BUILDER hs_render_character_texture_package
        RESULT "${texture_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/character/${mode}/result.d"
        ARGS --mode "${mode}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}")
    list(APPEND HS_CHARACTER_TEXTURE_RESULTS "${texture_result}")
endforeach()
add_custom_target(hs_render_character_texture_products DEPENDS ${HS_CHARACTER_TEXTURE_RESULTS})
add_executable(hs_vfx_content
    Source/Apps/VfxContent/main.cpp
    Source/Apps/Content/particle_effect_cooker.cpp
    Source/Apps/Content/vfx_spec_validation.cpp
    Source/Apps/Content/vfx_texture_validation.cpp
    Source/Apps/Content/vfx_gradient_cooker.cpp
    Source/Apps/Content/vfx_program_cooker.cpp
    Source/Apps/Content/vfx_mesh_cooker.cpp)
target_include_directories(hs_vfx_content PRIVATE Source/RendererD3D12/Public)
target_link_libraries(hs_vfx_content PRIVATE
    hs_asset_pipeline hs_content_json hs_render_data_values
    nlohmann_json::nlohmann_json DirectXTex ole32)
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
        BUILDER hs_render_vfx_texture_package
        RESULT "${texture_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/${texture_name}/result.d"
        UPSTREAM_RESULTS "${texture_upstream}"
        ARGS --build-root "${CMAKE_CURRENT_BINARY_DIR}" --name "${texture_name}")
    list(APPEND HS_RENDER_VFX_TEXTURE_RESULTS "${texture_result}")
endforeach()
set(HS_RUNTIME_FULL_RESULTS
    "${HS_COATED_DFG_RESULT}"
    "${HS_QUALITY_PROFILES_RESULT}"
    "${HS_VOLUMETRIC_MEDIA_RESULT}"
    "${HS_SCENE_HLOD_RESULT}"
    "${HS_SCENE_PROBE_RESULT}"
    "${HS_LIGHTING_PROBE_RESULT}"
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
if(HS_BUILD_RUNTIME)
    set(HS_TERRAIN_SURFACE_INPUT_RESULTS
        ${HS_SIMULATION_FINALIZER_RESULTS}
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/finalize-simulation/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/meshes/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/histogram/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/meadow_ground/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/terrain/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/material/terrain/parameters/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/terrain/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/runtime/terrain-page/result.json")
    foreach(channel IN ITEMS base_color surface_ra normal detail_normal height)
        list(APPEND HS_TERRAIN_SURFACE_INPUT_RESULTS
            "${CMAKE_CURRENT_BINARY_DIR}/Actions/environment/textures/terrain/${channel}/result.json")
    endforeach()
    foreach(channel IN ITEMS gaussian surface normal detail height histogram)
        list(APPEND HS_TERRAIN_SURFACE_INPUT_RESULTS
            "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/texture/environment_terrain_${channel}/result.json")
    endforeach()
    set(HS_TERRAIN_SURFACE_INPUT_LIST
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/hs-content/terrain-surface-input-results.txt")
    set(HS_TERRAIN_SURFACE_INPUT
        "${CMAKE_CURRENT_BINARY_DIR}/Generations/current/terrain-surface-input.json")
    hs_write_result_list("${HS_TERRAIN_SURFACE_INPUT_LIST}" ${HS_TERRAIN_SURFACE_INPUT_RESULTS})
    hs_compose_selection(terrain-surface-input "${HS_TERRAIN_SURFACE_INPUT}"
        "${HS_TERRAIN_SURFACE_INPUT_LIST}" HS_TERRAIN_SURFACE_INPUT_RESULTS)
    set(HS_TERRAIN_SURFACE_RESULT
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/terrain/surface/result.json")
    hs_add_asset_action(NAME hs_render_terrain_surface BUILDER hs_terrain_surface_bake
        RESULT "${HS_TERRAIN_SURFACE_RESULT}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/terrain/surface/result.d"
        STATIC_INPUTS "${HS_TERRAIN_SURFACE_INPUT}"
        ARGS --build-root "${CMAKE_CURRENT_BINARY_DIR}" --generation "${HS_TERRAIN_SURFACE_INPUT}")
    list(APPEND HS_RUNTIME_FULL_RESULTS "${HS_TERRAIN_SURFACE_RESULT}")

    # Keep the bake input independent of its output; runtime quality is not selected here.
    set(HS_STATIC_SCENE_INPUT_RESULTS
        ${HS_SIMULATION_FINALIZER_RESULTS}
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/finalize-simulation/result.json"
        ${HS_SCENE_RENDER_RESULTS})
    list(REMOVE_DUPLICATES HS_STATIC_SCENE_INPUT_RESULTS)
    set(HS_STATIC_SHADOW_INPUT_LIST
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/hs-content/static-shadow-input-results.txt")
    set(HS_STATIC_SHADOW_INPUT
        "${CMAKE_CURRENT_BINARY_DIR}/Generations/current/static-shadow-input.json")
    hs_write_result_list("${HS_STATIC_SHADOW_INPUT_LIST}" ${HS_STATIC_SCENE_INPUT_RESULTS})
    hs_compose_selection(static-shadow-input "${HS_STATIC_SHADOW_INPUT}"
        "${HS_STATIC_SHADOW_INPUT_LIST}" HS_STATIC_SCENE_INPUT_RESULTS)
    hs_add_asset_action(NAME hs_render_static_shadow_candidate BUILDER hs_static_shadow_bake
        RESULT "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/arena-static-shadow/result.json"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/arena-static-shadow/result.d"
        STATIC_INPUTS "${HS_STATIC_SHADOW_INPUT}"
        ARGS --build-root "${CMAKE_CURRENT_BINARY_DIR}" --generation "${HS_STATIC_SHADOW_INPUT}")
    list(APPEND HS_RUNTIME_FULL_RESULTS
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/arena-static-shadow/result.json")

    # Bake from the layout-only selection. The output Action replaces that
    # product only in runtime-full, after static-shadow input has been formed.
    set(HS_SCENE_PROBE_BAKE_INPUT
        "${CMAKE_CURRENT_BINARY_DIR}/Generations/current/scene-probe-bake-input.json")
    hs_compose_selection(scene-probe-bake-input "${HS_SCENE_PROBE_BAKE_INPUT}"
        "${HS_STATIC_SHADOW_INPUT_LIST}" HS_STATIC_SCENE_INPUT_RESULTS
        "${HS_SCENE_PROBE_RESULT}"
        "${HS_LIGHTING_PROBE_RESULT}"
        "${HS_QUALITY_PROFILES_RESULT}"
        "${HS_COATED_DFG_RESULT}"
        "${HS_TERRAIN_SURFACE_RESULT}"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/runtime/terrain-page/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/runtime/fullscreen/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/runtime/light-list/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/runtime/local-probe-bake/result.json")
    set(HS_SCENE_PROBE_BAKED_RESULT
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/scene-probes-baked/result.json")
    hs_add_asset_action(NAME hs_render_scene_probe_bake BUILDER hs_scene_probe_bake
        RESULT "${HS_SCENE_PROBE_BAKED_RESULT}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/lighting/scene-probes-baked/result.d"
        STATIC_INPUTS "${HS_SCENE_PROBE_BAKE_INPUT}"
        ARGS --source-root "${CMAKE_CURRENT_SOURCE_DIR}" --build-root "${CMAKE_CURRENT_BINARY_DIR}"
            --generation "${HS_SCENE_PROBE_BAKE_INPUT}")
    list(REMOVE_ITEM HS_RUNTIME_FULL_RESULTS "${HS_SCENE_PROBE_RESULT}")
    list(APPEND HS_RUNTIME_FULL_RESULTS "${HS_SCENE_PROBE_BAKED_RESULT}")
endif()
set(HS_RUNTIME_FULL_RESULT_LIST
    "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/hs-content/runtime-full-results.txt")
hs_write_result_list("${HS_RUNTIME_FULL_RESULT_LIST}" ${HS_RUNTIME_FULL_RESULTS})
set(HS_SHIPPING_INPUTS_STAMP
    "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/hs-content/shipping-inputs.sha256")
add_custom_command(
    OUTPUT "${HS_SHIPPING_INPUTS_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        "-DRESULT_LIST=${HS_RUNTIME_FULL_RESULT_LIST}"
        "-DOUTPUT=${HS_SHIPPING_INPUTS_STAMP}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShippingInputs.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShippingInputs.cmake"
        "${HS_RUNTIME_FULL_RESULT_LIST}" ${HS_RUNTIME_FULL_RESULTS}
    VERBATIM)
add_custom_command(
    OUTPUT "${HS_RUNTIME_DIRECTORY}/${HS_SHIPPING_PACK_RELATIVE_PATH}"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/${HS_SHIPPING_SOURCE_RELATIVE_PATH}"
    COMMAND "$<TARGET_FILE:hs_content_linker>" --build-root "${CMAKE_CURRENT_BINARY_DIR}"
        --font "${HS_FONT_DIRECTORY}/NotoSansKR.ttf"
        --renderer-result-list "${HS_RUNTIME_FULL_RESULT_LIST}"
    DEPENDS hs_content_linker "${HS_FONT_DIRECTORY}/NotoSansKR.ttf"
        "${HS_RUNTIME_FULL_RESULT_LIST}" "${HS_SHIPPING_INPUTS_STAMP}"
    VERBATIM)
add_custom_target(hs_shipping_image
    DEPENDS "${HS_RUNTIME_DIRECTORY}/${HS_SHIPPING_PACK_RELATIVE_PATH}")
hs_compose_selection(runtime-full "${CMAKE_CURRENT_BINARY_DIR}/Generations/current/runtime-full.json"
    "${HS_RUNTIME_FULL_RESULT_LIST}" HS_RUNTIME_FULL_RESULTS ${HS_TRANSLUCENCY_DIAGNOSTICS_SHADER_RESULT})
add_custom_target(hs_content_generation
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/Generations/current/runtime-full.json")
endif()

if(HS_BUILD_RUNTIME)
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/${HS_SHIPPING_SOURCE_RELATIVE_PATH}" PROPERTIES GENERATED TRUE)
    add_library(hs_shipping_content OBJECT EXCLUDE_FROM_ALL "${CMAKE_CURRENT_BINARY_DIR}/${HS_SHIPPING_SOURCE_RELATIVE_PATH}")
    target_link_libraries(hs_shipping_content PRIVATE hs_runtime_contracts)
    target_link_libraries(hs_shipping_content PUBLIC hs_core)
    if(HS_BUILD_CONTENT_TOOLS)
        add_dependencies(hs_shipping_content hs_shipping_image)
    endif()
    hs_configure_target(hs_shipping_content)
    target_link_libraries(ProjectHS PRIVATE hs_shipping_content)
    if(TARGET hs_experiment)
        target_link_libraries(hs_experiment PRIVATE hs_shipping_content)
    endif()
endif()

if(HS_BUILD_TESTS)
    set(CMAKE_TEST_BUILD_DEPENDS ON)
    enable_testing()
    add_executable(hs_tests Tests/foundation_tests.cpp)
    target_link_libraries(hs_tests PRIVATE hs_core hs_jobs)
    hs_configure_target(hs_tests)
    add_test(NAME foundation.core COMMAND hs_tests)
    add_test(NAME architecture.layers COMMAND ${CMAKE_COMMAND}
        -DBUILD_ROOT=${CMAKE_BINARY_DIR} -DCONFIG=$<CONFIG>
        -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_architecture.cmake)
    add_executable(hs_simulation_tests Tests/simulation_tests.cpp)
    target_link_libraries(hs_simulation_tests PRIVATE hs_core hs_game_rules hs_gameplay_development)
    hs_configure_target(hs_simulation_tests)
    add_test(NAME simulation.determinism-core COMMAND hs_simulation_tests)
    add_executable(hs_vfx_session_state_tests Tests/vfx_session_state_tests.cpp Tests/vfx_profile_admission_tests.cpp)
    target_link_libraries(hs_vfx_session_state_tests PRIVATE hs_runtime_contracts)
    target_include_directories(hs_vfx_session_state_tests PRIVATE Source/Runtime/Private)
    hs_configure_target(hs_vfx_session_state_tests)
    add_test(NAME runtime.vfx-session-state COMMAND hs_vfx_session_state_tests)
    add_executable(hs_ui_command_admission_tests Tests/ui_command_admission_tests.cpp)
    target_link_libraries(hs_ui_command_admission_tests PRIVATE hs_presentation)
    hs_configure_target(hs_ui_command_admission_tests)
    add_test(NAME runtime.ui-command-admission COMMAND hs_ui_command_admission_tests)
    add_executable(hs_render_graph_validation_tests Tests/render_graph_validation_tests.cpp
        Source/RendererD3D12/Private/render_graph_validation.cpp)
    target_include_directories(hs_render_graph_validation_tests PRIVATE Source/RendererD3D12/Private)
    target_link_libraries(hs_render_graph_validation_tests PRIVATE hs_core hs_render_data_values)
    target_compile_definitions(hs_render_graph_validation_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    hs_configure_target(hs_render_graph_validation_tests)
    add_test(NAME rendering.graph-range-validation COMMAND hs_render_graph_validation_tests)
    set_tests_properties(rendering.graph-range-validation PROPERTIES TIMEOUT 30)
    if(HS_BUILD_CONTENT_TOOLS)
        if(CMAKE_GENERATOR MATCHES "Ninja")
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            add_executable(hs_generation_reader_probe Tests/generation_reader_probe.cpp)
            target_link_libraries(hs_generation_reader_probe PRIVATE hs_asset_manifest hs_asset_pipeline nlohmann_json::nlohmann_json)
            hs_configure_target(hs_generation_reader_probe)
            add_test(NAME content.generation-lifetime COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Tests/check_generation_lifetime.py"
                --generation "$<TARGET_FILE:hs_generation>" --probe "$<TARGET_FILE:hs_generation_reader_probe>"
                BUILD_DEPENDS hs_generation hs_generation_reader_probe)
        endif()
        add_executable(hs_gameplay_tests Tests/gameplay_tests.cpp Tests/gameplay_test_support.cpp
            Tests/gameplay_combat_tests.cpp Tests/gameplay_skill_tests.cpp Tests/gameplay_upgrade_tests.cpp
            Tests/gameplay_relic_tests.cpp Tests/gameplay_boss_tests.cpp Tests/gameplay_progression_tests.cpp
            Tests/gameplay_presentation_tests.cpp)
        target_link_libraries(hs_gameplay_tests PRIVATE hs_core hs_gameplay_development
            hs_game_rules_io hs_presentation hs_presentation_io hs_asset_pipeline
            nlohmann_json::nlohmann_json)
        hs_configure_target(hs_gameplay_tests)
        foreach(suite IN ITEMS combat progression presentation)
            if(suite STREQUAL "presentation")
                set(test_name simulation.presentation-contract)
            else()
                set(test_name simulation.${suite})
            endif()
            add_test(NAME ${test_name} COMMAND hs_gameplay_tests ${suite}
                BUILD_DEPENDS
                    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/simulation-payload/result.json"
                    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/presentation-payload/result.json")
            set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
        endforeach()
    endif()
    if(HS_BUILD_RUNTIME)
        add_executable(hs_residency_streaming_tests Tests/residency_streaming_tests.cpp)
        target_link_libraries(hs_residency_streaming_tests PRIVATE hs_renderer_d3d12 D3D12MemoryAllocator)
        target_compile_definitions(hs_residency_streaming_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_include_directories(hs_residency_streaming_tests PRIVATE Source/RendererD3D12/Private)
        hs_configure_target(hs_residency_streaming_tests)
        add_dependencies(hs_residency_streaming_tests hs_runtime_native_assets)
        add_test(NAME rendering.residency-streaming COMMAND hs_residency_streaming_tests)
        add_executable(hs_render_graph_multi_queue_tests Tests/render_graph_multi_queue_tests.cpp)
        target_link_libraries(hs_render_graph_multi_queue_tests PRIVATE
            hs_renderer_d3d12 hs_render_data hs_jobs D3D12MemoryAllocator d3d12 dxgi)
        target_include_directories(hs_render_graph_multi_queue_tests PRIVATE Source/RendererD3D12/Private)
        target_compile_definitions(hs_render_graph_multi_queue_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        hs_configure_target(hs_render_graph_multi_queue_tests)
        add_dependencies(hs_render_graph_multi_queue_tests hs_runtime_native_assets)
        add_test(NAME rendering.multi-queue-graph COMMAND hs_render_graph_multi_queue_tests)
        set_tests_properties(rendering.multi-queue-graph PROPERTIES TIMEOUT 90 RESOURCE_LOCK projecths_d3d12)
        add_test(NAME rendering.cluster-retest COMMAND hs_render_test_runner
            --frames=24
            --surface=visibility --geometry=indexed --barriers=enhanced --gpu-validation
            --persistent-scene-probe --require-multi-queue
            BUILD_DEPENDS hs_render_test_runner)
        add_test(NAME rendering.residency-streaming-gpu COMMAND hs_render_test_runner
            --frames=48
            --surface=compact --geometry=indexed --barriers=enhanced --gpu-validation --residency-streaming-probe
            BUILD_DEPENDS hs_render_test_runner)
        add_test(NAME rendering.owned-view COMMAND hs_render_test_runner
            --frames=3
            --surface=visibility --geometry=indexed --barriers=enhanced --gpu-validation --owned-view-probe
            BUILD_DEPENDS hs_render_test_runner)
        set_tests_properties(rendering.cluster-retest rendering.residency-streaming-gpu rendering.owned-view
            PROPERTIES TIMEOUT 600 RESOURCE_LOCK projecths_d3d12)
        add_executable(hs_runtime_tests Tests/runtime_tests.cpp Tests/graphics_settings_tests.cpp)
        target_link_libraries(hs_runtime_tests PRIVATE hs_runtime hs_presentation)
        target_compile_definitions(hs_runtime_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX $<$<NOT:$<CONFIG:Release>>:HS_DEVELOPMENT_TOOLS=1>)
        target_include_directories(hs_runtime_tests PRIVATE Source/Runtime/Private)
        hs_configure_target(hs_runtime_tests)
        add_test(NAME runtime.integration COMMAND hs_runtime_tests)
        set_tests_properties(runtime.integration PROPERTIES TIMEOUT 30)
    endif()

    get_property(HS_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
    foreach(test_name IN LISTS HS_REGISTERED_TESTS)
        get_property(test_labels TEST ${test_name} PROPERTY LABELS)
        if(test_name MATCHES "^(foundation|architecture)\\.")
            list(APPEND test_labels core)
        endif()
        if(test_name MATCHES "^simulation\\.")
            list(APPEND test_labels gameplay)
        endif()
        if(test_name MATCHES "^content\\.")
            list(APPEND test_labels content)
        endif()
        if(test_name MATCHES "^runtime\\.")
            list(APPEND test_labels runtime)
        endif()
        if(test_name MATCHES "^rendering\\.")
            list(APPEND test_labels render)
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
if(NOT HS_SINGLE_CONFIG_RELEASE)
    install(
        FILES "${winpix_SOURCE_DIR}/bin/x64/WinPixEventRuntime.dll"
        DESTINATION .
        CONFIGURATIONS Debug RelWithDebInfo MinSizeRel
        COMPONENT Runtime
    )
endif()
install(
    FILES "${HS_FONT_DIRECTORY}/NotoSansKR-OFL.txt"
    DESTINATION Licenses
    COMPONENT Runtime
)
get_filename_component(hs_pack_directory "${HS_SHIPPING_PACK_RELATIVE_PATH}" DIRECTORY)
install(FILES "${HS_RUNTIME_DIRECTORY}/${HS_SHIPPING_PACK_RELATIVE_PATH}"
    DESTINATION "${hs_pack_directory}" COMPONENT Runtime)
install(FILES ThirdPartyNotices.txt DESTINATION . COMPONENT Runtime)
if(HS_SINGLE_CONFIG_RELEASE)
    install(CODE
        "set(_hs_release_source_exe \"$<TARGET_FILE:ProjectHS>\")\nset(_hs_release_source_pack \"${HS_RUNTIME_DIRECTORY}/${HS_SHIPPING_PACK_RELATIVE_PATH}\")"
        COMPONENT Runtime)
    install(SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_release_finish.cmake" COMPONENT Runtime)
endif()

endif()

# Test preparation requests these hosts only when a selected case uses them.
foreach(hs_development_host IN ITEMS hs_experiment hs_render_test_runner hs_combat_sim)
    if(TARGET ${hs_development_host})
        set_property(TARGET ${hs_development_host} PROPERTY EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()

# D3D12 hosts load Agility and optional renderer DLLs relative to the process image.
foreach(host IN ITEMS ProjectHS hs_experiment hs_render_test_runner hs_scene_probe_bake
        hs_static_shadow_bake hs_terrain_surface_bake hs_render_graph_multi_queue_tests
        hs_residency_streaming_tests)
    if(TARGET ${host})
        set_property(TARGET ${host} PROPERTY RUNTIME_OUTPUT_DIRECTORY "${HS_RUNTIME_DIRECTORY}")
    endif()
endforeach()
