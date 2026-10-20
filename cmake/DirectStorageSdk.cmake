# Runtime-only optional SDK. Never invoked by the low-level or CPU cooker path.
function(hs_configure_directstorage_sdk target)
    if(NOT HS_WITH_DIRECTSTORAGE)
        target_compile_definitions(${target} PRIVATE HS_WITH_DIRECTSTORAGE=0)
        return()
    endif()
    find_program(HS_DIRECTSTORAGE_POWERSHELL NAMES pwsh REQUIRED)
    execute_process(COMMAND "${HS_DIRECTSTORAGE_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/Tools/bootstrap_render_sdks.ps1" -Sdk directstorage -Offline -PrintPath
        RESULT_VARIABLE verified OUTPUT_VARIABLE sdk ERROR_VARIABLE failure OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT verified EQUAL 0)
        message(FATAL_ERROR "Pinned DirectStorage 1.3.0 verification failed: ${failure}")
    endif()
    file(TO_CMAKE_PATH "${sdk}" sdk)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/RenderSDKs.lock.json" "${CMAKE_SOURCE_DIR}/Tools/bootstrap_render_sdks.ps1")
    add_library(hs_directstorage_sdk STATIC IMPORTED GLOBAL)
    set_property(TARGET hs_directstorage_sdk PROPERTY IMPORTED_LOCATION "${sdk}/native/lib/x64/dstorage.lib")
    target_link_libraries(${target} PRIVATE hs_directstorage_sdk)
    target_compile_definitions(${target} PRIVATE HS_WITH_DIRECTSTORAGE=1)
    set_property(SOURCE Source/RendererD3D12/Private/renderer_asset_io.cpp APPEND PROPERTY
        INCLUDE_DIRECTORIES "${sdk}/native/include")
    foreach(runtime IN ITEMS dstorage.dll dstoragecore.dll)
        install(FILES "${sdk}/native/bin/x64/${runtime}" DESTINATION . COMPONENT Runtime)
        add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${sdk}/native/bin/x64/${runtime}" "$<TARGET_FILE_DIR:${target}>/${runtime}" VERBATIM)
    endforeach()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:${target}>/Licenses/DirectStorage-1.3.0" VERBATIM)
    foreach(notice IN ITEMS LICENSE.txt LICENSE-CODE.txt NOTICES.txt)
        install(FILES "${sdk}/${notice}" DESTINATION "Licenses/DirectStorage-1.3.0" COMPONENT Runtime)
        add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${sdk}/${notice}" "$<TARGET_FILE_DIR:${target}>/Licenses/DirectStorage-1.3.0/${notice}" VERBATIM)
    endforeach()
endfunction()
