file(GLOB HS_AUDIO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Audio/*.wav"
)
file(GLOB_RECURSE HS_ENVIRONMENT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/Environment/*.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/ContentSource/Textures/Environment/*.json"
)

set(HS_SIMULATION_FINALIZER_RESULTS
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/simulation-payload/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/gameplay-identity/result.json")
set(HS_PRESENTATION_FINALIZER_RESULTS
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/presentation-payload/result.json"
    "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/content-identity/result.json")

add_executable(hs_game_data
    Source/Apps/GameData/main.cpp
    Source/Apps/Content/game_data_cooker.cpp
    Source/Apps/Content/game_data_validation.cpp
    Source/Apps/Content/content_source_inventory.cpp
    Source/Apps/Content/content_paths.cpp
)
target_link_libraries(hs_game_data PRIVATE hs_asset_pipeline hs_presentation_io hs_game_rules_io)
target_include_directories(hs_game_data PRIVATE Source/Apps/Content)
target_link_libraries(hs_game_data PRIVATE
    hs_core hs_game_rules hs_presentation hs_content_json nlohmann_json::nlohmann_json bcrypt)
target_compile_definitions(hs_game_data PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    HS_CONTENT_SOURCE_ROOT=u8"${CMAKE_CURRENT_SOURCE_DIR}")
hs_configure_target(hs_game_data)

foreach(mode IN ITEMS simulation-payload presentation-payload gameplay-identity content-identity finalize-simulation finalize-presentation)
    set(action_result "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/${mode}/result.json")
    set(action_inputs)
    set(action_upstream)
    # Fixed source inputs are recorded by the producer's depfile. The full
    # identity also discovers new environment files and WAVs by directory scan.
    if(mode STREQUAL "content-identity")
        set(action_inputs ${HS_ENVIRONMENT_SOURCES} ${HS_AUDIO_SOURCES})
    elseif(mode STREQUAL "finalize-simulation")
        set(action_upstream ${HS_SIMULATION_FINALIZER_RESULTS})
    elseif(mode STREQUAL "finalize-presentation")
        set(action_upstream ${HS_PRESENTATION_FINALIZER_RESULTS})
    endif()
    hs_add_asset_action(
        BUILDER hs_game_data
        RESULT "${action_result}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/${mode}/result.d"
        STATIC_INPUTS ${action_inputs}
        UPSTREAM_RESULTS ${action_upstream}
        ARGS "--${mode}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}"
    )
endforeach()
add_custom_target(hs_game_data_products
    DEPENDS
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/finalize-simulation/result.json"
        "${CMAKE_CURRENT_BINARY_DIR}/Actions/game-data/finalize-presentation/result.json")
