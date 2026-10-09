# Optional native renderer SDKs are isolated from the low-level module graph.
include("${CMAKE_SOURCE_DIR}/cmake/RenderSdkPin.cmake")
option(HS_WITH_NRD "Build pinned NVIDIA NRD native denoising" OFF)
set(HS_NRD_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Build/tooling/render-sdks/nrd-4.17.3"
    CACHE PATH "Verified locally built NVIDIA NRD SDK directory")

function(hs_verify_nrd_sdk)
    find_program(HS_SDK_POWERSHELL NAMES pwsh powershell REQUIRED)
    execute_process(COMMAND "${HS_SDK_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/bootstrap_render_sdks.ps1"
        -Sdk nrd -Offline -PrintPath
        RESULT_VARIABLE result OUTPUT_VARIABLE verified_root ERROR_VARIABLE failure
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Pinned NRD source/native build verification failed. Build the explicit NRD prerequisite first.\n${failure}")
    endif()
    file(REAL_PATH "${HS_NRD_SDK_ROOT}" configured_root)
    file(REAL_PATH "${verified_root}" verified_root)
    if(NOT configured_root STREQUAL verified_root)
        message(FATAL_ERROR "HS_NRD_SDK_ROOT must identify ${verified_root}")
    endif()
endfunction()

function(hs_configure_render_sdks)
    option(HS_WITH_XESS "Build pinned XeSS super resolution" OFF)
    option(HS_WITH_XESS_FG "Build pinned XeSS frame generation" OFF)
    option(HS_WITH_XELL "Build pinned Intel XeLL latency control" OFF)
    option(HS_WITH_STREAMLINE "Build pinned NVIDIA Streamline adapters" OFF)
    option(HS_WITH_FSR "Build pinned AMD FSR adapters" OFF)
    option(HS_WITH_DIRECTSR "Build isolated pinned DirectSR preview adapter for local development" OFF)
    if(HS_WITH_XESS_FG AND NOT HS_WITH_XELL)
        message(FATAL_ERROR "XeSS frame generation requires HS_WITH_XELL.")
    endif()
    set(HS_XESS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Build/tooling/render-sdks/xess-3.0.2"
        CACHE PATH "Verified Intel XeSS SDK directory")
    set(HS_STREAMLINE_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Build/tooling/render-sdks/streamline-2.14.1"
        CACHE PATH "Verified NVIDIA Streamline SDK directory")
    set(HS_FSR_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Build/tooling/render-sdks/fsr-2.3.0"
        CACHE PATH "Verified AMD FSR SDK directory")
    set(HS_DIRECTSR_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Build/tooling/render-sdks/directsr-1.715.1-preview"
        CACHE PATH "Verified DirectSR preview SDK directory")
    target_sources(hs_renderer_d3d12 PRIVATE
        Source/RendererD3D12/Private/renderer_xess_fg.cpp
        Source/RendererD3D12/Private/renderer_xell.cpp
        Source/RendererD3D12/Private/renderer_streamline.cpp
        Source/RendererD3D12/Private/renderer_fsr.cpp
        Source/RendererD3D12/Private/renderer_nrd.cpp
        Source/RendererD3D12/Private/renderer_directsr.cpp
        Source/RendererD3D12/Private/renderer_sdk_presentation.cpp
        Source/RendererD3D12/Private/presentation-guides.cpp)
    foreach(feature IN ITEMS XESS XESS_FG XELL STREAMLINE FSR NRD DIRECTSR)
        target_compile_definitions(hs_renderer_d3d12 PRIVATE HS_WITH_${feature}=$<BOOL:${HS_WITH_${feature}}>)
    endforeach()
    set(enabled)
    if(HS_WITH_NRD)
        hs_verify_nrd_sdk()
        list(APPEND enabled nrd)
        target_include_directories(hs_renderer_d3d12 PRIVATE "${HS_NRD_SDK_ROOT}/Include")
    endif()
    if(HS_WITH_DIRECTSR)
        list(APPEND enabled directsr)
        # Bootstrap extracts only directsr.h from this directory. The main
        # renderer continues using the existing Agility 619 D3D12 headers.
        target_include_directories(hs_renderer_d3d12 PRIVATE "${HS_DIRECTSR_SDK_ROOT}/build/native/include")
    endif()
    if(HS_WITH_XESS OR HS_WITH_XESS_FG OR HS_WITH_XELL)
        list(APPEND enabled xess)
        target_include_directories(hs_renderer_d3d12 PRIVATE "${HS_XESS_SDK_ROOT}/inc")
    endif()
    if(HS_WITH_STREAMLINE)
        list(APPEND enabled streamline)
        target_include_directories(hs_renderer_d3d12 PRIVATE "${HS_STREAMLINE_SDK_ROOT}/include")
        target_link_libraries(hs_renderer_d3d12 PRIVATE wintrust crypt32)
    endif()
    if(HS_WITH_FSR)
        list(APPEND enabled fsr)
        foreach(component IN ITEMS api upscalers framegeneration denoisers)
            target_include_directories(hs_renderer_d3d12 PRIVATE "${HS_FSR_SDK_ROOT}/Kits/FidelityFX/${component}/include")
        endforeach()
        target_link_libraries(hs_renderer_d3d12 PRIVATE wintrust crypt32)
    endif()
    if(enabled)
        find_program(HS_SDK_POWERSHELL NAMES pwsh powershell REQUIRED)
    endif()
    foreach(sdk IN LISTS enabled)
        if(sdk STREQUAL "nrd")
            continue()
        endif()
        execute_process(COMMAND "${HS_SDK_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/bootstrap_render_sdks.ps1"
            -Sdk "${sdk}" -Offline -PrintPath
            RESULT_VARIABLE result OUTPUT_VARIABLE verified_root ERROR_VARIABLE failure
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Pinned ${sdk} SDK verification failed. Run tools/bootstrap_render_sdks.ps1 -Sdk ${sdk}.\n${failure}")
        endif()
        string(TOUPPER "${sdk}" sdk_variable)
        file(REAL_PATH "${HS_${sdk_variable}_SDK_ROOT}" configured_root)
        file(REAL_PATH "${verified_root}" verified_root)
        if(NOT configured_root STREQUAL verified_root)
            message(FATAL_ERROR "HS_${sdk_variable}_SDK_ROOT must identify the verified pinned bootstrap directory: ${verified_root}")
        endif()
    endforeach()
    set(HS_ENABLED_RENDER_SDKS "${enabled}" PARENT_SCOPE)
endfunction()

function(hs_deploy_render_sdks)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/RenderSDKs.lock.json" sdk_lock)
    set(native_outputs)
    foreach(sdk IN LISTS HS_ENABLED_RENDER_SDKS)
        string(TOUPPER "${sdk}" sdk_variable)
        set(sdk_root "${HS_${sdk_variable}_SDK_ROOT}")
        foreach(kind IN ITEMS runtime notices)
            string(JSON component_count LENGTH "${sdk_lock}" sdks ${sdk} ${kind})
            if(component_count LESS 1)
                continue()
            endif()
            math(EXPR component_last "${component_count} - 1")
            foreach(index RANGE 0 ${component_last})
                string(JSON relative GET "${sdk_lock}" sdks ${sdk} ${kind} ${index})
                get_filename_component(filename "${relative}" NAME)
                if(kind STREQUAL "notices")
                    set(destination "Licenses/${sdk}")
                elseif(sdk STREQUAL "directsr")
                    set(destination "DirectSR")
                elseif(sdk STREQUAL "xess" OR sdk STREQUAL "nrd")
                    set(destination ".")
                else()
                    set(destination "${sdk}")
                endif()
                set(output "${CMAKE_CURRENT_BINARY_DIR}/${destination}/${filename}")
                hs_copy_runtime_asset("${sdk_root}/${relative}" "${output}")
                # The preview license does not establish redistribution rights.
                # Local build deployment is supported; exclude it from installs.
                if(NOT sdk STREQUAL "directsr")
                    install(FILES "${sdk_root}/${relative}" DESTINATION "${destination}" COMPONENT Runtime)
                endif()
                if(kind STREQUAL "runtime")
                    list(APPEND native_outputs "${output}")
                endif()
            endforeach()
        endforeach()
    endforeach()
    set(HS_RUNTIME_ASSET_OUTPUTS "${HS_RUNTIME_ASSET_OUTPUTS}" PARENT_SCOPE)
    set(HS_RENDER_SDK_NATIVE_OUTPUTS "${native_outputs}" PARENT_SCOPE)
endfunction()

# Only called from the content-tools branch, after DXC discovery. The producer
# reads CPU metadata from NRD.dll; it neither creates a D3D12 device nor compiles
# SDK source during the main build.
function(hs_add_nrd_shader_action)
    if(NOT HS_WITH_NRD)
        return()
    endif()
    hs_verify_nrd_sdk()
    add_executable(hs_render_nrd_shader
        Source/Apps/RenderNrdShader/main.cpp
        Source/Apps/AssetPipeline/action.cpp)
    target_link_libraries(hs_render_nrd_shader PRIVATE hs_core nlohmann_json::nlohmann_json bcrypt)
    target_include_directories(hs_render_nrd_shader PRIVATE "${HS_NRD_SDK_ROOT}/Include")
    target_compile_definitions(hs_render_nrd_shader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    hs_configure_target(hs_render_nrd_shader)
    set(result "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/nrd/result.json")
    hs_render_sdk_pin(nrd nrd_pin)
    hs_add_asset_action(NAME hs_render_nrd_shader_action BUILDER hs_render_nrd_shader
        RESULT "${result}" DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/Actions/render/shader/nrd/result.d"
        STATIC_INPUTS "${nrd_pin}"
            "${HS_NRD_SDK_ROOT}/native-build-receipt.json"
            "${HS_NRD_SDK_ROOT}/bin/NRD.dll"
            "${HS_NRD_SDK_ROOT}/generated/NRDConfig.hlsli"
            "${HS_NRD_SDK_ROOT}/Shaders/NRD.hlsli" ${HS_DXC_TOOL_INPUTS}
        ARGS "--source=${CMAKE_CURRENT_SOURCE_DIR}" "--build=${CMAKE_CURRENT_BINARY_DIR}"
            "--sdk=${HS_NRD_SDK_ROOT}" "--dxc=${HS_DXC}" "--pin=${nrd_pin}")
    set(HS_RENDER_SHADER_RESULTS "${HS_RENDER_SHADER_RESULTS};${result}" PARENT_SCOPE)
endfunction()
