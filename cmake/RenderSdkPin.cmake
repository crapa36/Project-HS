include_guard(GLOBAL)

# Keep the full lock authoritative and a configure dependency. Actions consume
# only a complete selected entry so unrelated SDK pins cannot dirty their work.
function(hs_render_sdk_pin sdk output)
    set(lock "${CMAKE_SOURCE_DIR}/RenderSDKs.lock.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${lock}")
    file(READ "${lock}" document)
    string(JSON schema GET "${document}" schema_version)
    if(NOT schema EQUAL 1)
        message(FATAL_ERROR "Unsupported renderer SDK lock schema")
    endif()
    string(JSON entry GET "${document}" sdks "${sdk}")
    set(projection "${CMAKE_BINARY_DIR}/render-sdk-pins/${sdk}.json")
    # file(GENERATE) updates the file only when its exact content changes.
    file(GENERATE OUTPUT "${projection}"
        CONTENT "{\n  \"schema_version\": ${schema},\n  \"sdk\": \"${sdk}\",\n  \"entry\": ${entry}\n}\n")
    set(${output} "${projection}" PARENT_SCOPE)
endfunction()
