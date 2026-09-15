if(NOT DEFINED MAKE_PROGRAM OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "MAKE_PROGRAM and BINARY_DIR are required")
endif()

execute_process(
    COMMAND "${MAKE_PROGRAM}" -C "${BINARY_DIR}" -t deps
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "ninja -t deps failed (${result}):\n${error_output}")
endif()

set(expected_objects
    game_simulation
    simulation_combat
    simulation_progression
    simulation_tick
)
set(found_objects "")
set(current_object "")
set(current_deps 0)
set(current_has_world_header FALSE)

string(REPLACE "\r\n" "\n" output "${output}")
string(REPLACE "\n" ";" output_lines "${output}")
foreach(line IN LISTS output_lines)
    if(line MATCHES "^CMakeFiles/hs_gameplay\\.dir/Source/Gameplay/Private/([^:]+)\\.cpp\\.obj: #deps ([0-9]+)")
        if(NOT current_object STREQUAL "")
            if(current_deps LESS 1 OR NOT current_has_world_header)
                message(FATAL_ERROR
                    "${current_object} has incomplete Ninja dependency data: "
                    "deps=${current_deps}, simulation_world.hpp=${current_has_world_header}"
                )
            endif()
            list(APPEND found_objects "${current_object}")
        endif()
        set(current_object "${CMAKE_MATCH_1}")
        set(current_deps "${CMAKE_MATCH_2}")
        set(current_has_world_header FALSE)
    elseif(line MATCHES "^[^:]+: #deps [0-9]+")
        if(NOT current_object STREQUAL "")
            if(current_deps LESS 1 OR NOT current_has_world_header)
                message(FATAL_ERROR
                    "${current_object} has incomplete Ninja dependency data: "
                    "deps=${current_deps}, simulation_world.hpp=${current_has_world_header}"
                )
            endif()
            list(APPEND found_objects "${current_object}")
        endif()
        set(current_object "")
        set(current_deps 0)
        set(current_has_world_header FALSE)
    elseif(NOT current_object STREQUAL "" AND line MATCHES "simulation_world\\.hpp")
        set(current_has_world_header TRUE)
    endif()
endforeach()

if(NOT current_object STREQUAL "")
    if(current_deps LESS 1 OR NOT current_has_world_header)
        message(FATAL_ERROR
            "${current_object} has incomplete Ninja dependency data: "
            "deps=${current_deps}, simulation_world.hpp=${current_has_world_header}"
        )
    endif()
    list(APPEND found_objects "${current_object}")
endif()

foreach(expected IN LISTS expected_objects)
    list(FIND found_objects "${expected}" index)
    if(index EQUAL -1)
        message(FATAL_ERROR
            "Ninja dependency data for Gameplay/${expected}.cpp is missing. "
            "Reconfigure with 'cmake --fresh --preset <msvc-preset>' and rebuild."
        )
    endif()
endforeach()

message(STATUS "MSVC/Ninja header dependencies verified for ${found_objects}")
