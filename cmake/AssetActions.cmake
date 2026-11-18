function(hs_add_asset_action)
    cmake_parse_arguments(A "" "NAME;BUILDER;RESULT;DEPFILE" "STATIC_INPUTS;UPSTREAM_RESULTS;ARGS" ${ARGN})
    foreach(required BUILDER RESULT DEPFILE)
        if(NOT A_${required})
            message(FATAL_ERROR "hs_add_asset_action requires ${required}")
        endif()
    endforeach()
    if(A_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "hs_add_asset_action(${A_RESULT}): unknown arguments: ${A_UNPARSED_ARGUMENTS}")
    endif()
    list(REMOVE_DUPLICATES A_STATIC_INPUTS)
    add_custom_command(
        OUTPUT "${A_RESULT}"
        COMMAND "$<TARGET_FILE:${A_BUILDER}>" ${A_ARGS}
        DEPENDS ${A_BUILDER} ${A_STATIC_INPUTS} ${A_UPSTREAM_RESULTS}
        DEPFILE "${A_DEPFILE}"
        BYPRODUCTS "${A_DEPFILE}"
        VERBATIM
    )
    if(A_NAME)
        add_custom_target(${A_NAME} DEPENDS "${A_RESULT}")
    endif()
endfunction()

function(hs_write_result_list result_list)
    list(JOIN ARGN "\n" result_lines)
    file(GENERATE OUTPUT "${result_list}" CONTENT "${result_lines}\n")
endfunction()

function(hs_compose_selection scope manifest result_list listed_results)
    set(extra_args)
    foreach(result IN LISTS ARGN)
        list(APPEND extra_args --result "${result}")
    endforeach()
    add_custom_command(
        OUTPUT "${manifest}"
        COMMAND "$<TARGET_FILE:hs_generation>" --compose
            --build-root "${CMAKE_CURRENT_BINARY_DIR}" --scope "${scope}"
            --result-list "${result_list}" ${extra_args}
        DEPENDS hs_generation "${result_list}" ${${listed_results}} ${ARGN}
        VERBATIM)
endfunction()
