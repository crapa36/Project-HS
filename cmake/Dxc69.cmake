include_guard(GLOBAL)
include("${CMAKE_SOURCE_DIR}/cmake/RenderSdkPin.cmake")

# Content tools only. Ordinary shaders retain HS_DXC and their existing ABI.
function(hs_configure_dxc69)
    find_program(HS_DXC69_POWERSHELL NAMES pwsh powershell REQUIRED)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/RenderSDKs.lock.json"
        "${CMAKE_SOURCE_DIR}/Tools/bootstrap_render_sdks.ps1")
    execute_process(COMMAND "${HS_DXC69_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/Tools/bootstrap_render_sdks.ps1"
        -Sdk dxc69 -Offline -PrintPath
        RESULT_VARIABLE verified OUTPUT_VARIABLE sdk ERROR_VARIABLE failure
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT verified EQUAL 0)
        message(FATAL_ERROR "Pinned retail DXC 6.9 prerequisite unavailable. Run Tools/bootstrap_render_sdks.ps1 -Sdk dxc69.\n${failure}")
    endif()
    set(HS_DXC69 "${sdk}/bin/x64/dxc.exe" CACHE FILEPATH "Pinned retail native-ray DXC" FORCE)
    hs_render_sdk_pin(dxc69 pin)
    set(inputs "${HS_DXC69}" "${sdk}/bin/x64/dxcompiler.dll"
        "${sdk}/bin/x64/dxil.dll" "${sdk}/bin/x64/dxv.exe"
        "${sdk}/LICENSE-LLVM.txt" "${sdk}/LICENSE-MIT.txt"
        "${sdk}/LICENSE-MS.txt" "${sdk}/inc/hlsl/LICENSE.txt")
    set(HS_DXC69_PIN "${pin}" PARENT_SCOPE)
    set(HS_DXC69_ROOT "${sdk}" PARENT_SCOPE)
    set(HS_DXC69_TOOL_INPUTS "${inputs}" PARENT_SCOPE)
endfunction()
