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

# Runtime/renderer translation units share the private D3D12Renderer::Impl
# layout. Missing Ninja header dependencies here can leave a mixed-layout
# static library after a private header edit, which is substantially worse than
# an ordinary stale object: construction may succeed in one TU and crash when
# another TU reads the same object. Only require these entries when the renderer
# target exists in this configured tree so verify-core remains runtime-free.
set(renderer_expected_objects
    renderer
    renderer_device
    renderer_resources
    renderer_pipeline
    renderer_environment
    renderer_environment_mesh
    renderer_capture
    renderer_diagnostics
    renderer_ui
    renderer_frame
)
set(renderer_found_objects "")
set(renderer_seen FALSE)
set(current_renderer_object "")
set(current_renderer_deps 0)
set(current_renderer_has_impl FALSE)
set(current_renderer_has_resources FALSE)

macro(finalize_renderer_object)
    if(NOT current_renderer_object STREQUAL "")
        list(FIND renderer_expected_objects "${current_renderer_object}" renderer_expected_index)
        if(NOT renderer_expected_index EQUAL -1)
            if(current_renderer_deps LESS 1 OR
               NOT current_renderer_has_impl OR
               NOT current_renderer_has_resources)
                message(FATAL_ERROR
                    "${current_renderer_object} has incomplete Ninja dependency data: "
                    "deps=${current_renderer_deps}, renderer_impl.hpp=${current_renderer_has_impl}, "
                    "d3d12_resources.hpp=${current_renderer_has_resources}"
                )
            endif()
            list(APPEND renderer_found_objects "${current_renderer_object}")
        endif()
    endif()
    set(current_renderer_object "")
    set(current_renderer_deps 0)
    set(current_renderer_has_impl FALSE)
    set(current_renderer_has_resources FALSE)
endmacro()

foreach(line IN LISTS output_lines)
    if(line MATCHES "^CMakeFiles/hs_renderer_d3d12\\.dir/Source/RendererD3D12/Private/([^:]+)\\.cpp\\.obj: #deps ([0-9]+)")
        finalize_renderer_object()
        set(renderer_seen TRUE)
        set(current_renderer_object "${CMAKE_MATCH_1}")
        set(current_renderer_deps "${CMAKE_MATCH_2}")
    elseif(line MATCHES "^[^:]+: #deps [0-9]+")
        finalize_renderer_object()
    elseif(NOT current_renderer_object STREQUAL "" AND line MATCHES "renderer_impl\\.hpp")
        set(current_renderer_has_impl TRUE)
    elseif(NOT current_renderer_object STREQUAL "" AND line MATCHES "d3d12_resources\\.hpp")
        set(current_renderer_has_resources TRUE)
    endif()
endforeach()
finalize_renderer_object()

if(renderer_seen)
    foreach(expected IN LISTS renderer_expected_objects)
        list(FIND renderer_found_objects "${expected}" index)
        if(index EQUAL -1)
            message(FATAL_ERROR
                "Ninja dependency data for RendererD3D12/${expected}.cpp is missing. "
                "Reconfigure with 'cmake --fresh --preset <msvc-preset>' and rebuild."
            )
        endif()
    endforeach()
endif()

message(STATUS
    "MSVC/Ninja header dependencies verified for gameplay=${found_objects}; "
    "renderer=${renderer_found_objects}"
)
