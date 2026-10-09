# Effect-specific FidelityFX 1.1.4 host/backend and exact official blob tables.
# Call only from Runtime/content-tools branches. No low-level prerequisite.
function(hs_add_brixelizer_sdk)
    if(TARGET hs_brixelizer_native)
        return()
    endif()
    find_program(HS_BRIXELIZER_POWERSHELL NAMES pwsh REQUIRED)
    execute_process(COMMAND "${HS_BRIXELIZER_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/Tools/bootstrap-brixelizer-sdk.ps1" -Offline -PrintPath
        RESULT_VARIABLE verified OUTPUT_VARIABLE sdk ERROR_VARIABLE failure OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT verified EQUAL 0)
        message(FATAL_ERROR "Pinned Brixelizer SDK verification failed: ${failure}")
    endif()
    set(HS_BRIXELIZER_SDK_ROOT "${sdk}" CACHE PATH "Verified FidelityFX 1.1.4 source snapshot" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/cmake/BrixelizerNative" "${CMAKE_BINARY_DIR}/brixelizer-native")
endfunction()
