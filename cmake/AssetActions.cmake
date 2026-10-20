function(hs_add_asset_action)
    cmake_parse_arguments(A "" "NAME;BUILDER;RESULT;DEPFILE" "STATIC_INPUTS;UPSTREAM_RESULTS;ARGS" ${ARGN})
    foreach(required NAME BUILDER RESULT DEPFILE)
        if(NOT A_${required})
            message(FATAL_ERROR "hs_add_asset_action requires ${required}")
        endif()
    endforeach()
    if(A_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "hs_add_asset_action(${A_NAME}): unknown arguments: ${A_UNPARSED_ARGUMENTS}")
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
    add_custom_target(${A_NAME} DEPENDS "${A_RESULT}")
endfunction()
