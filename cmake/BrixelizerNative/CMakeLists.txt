set(brix_sdk "${HS_BRIXELIZER_SDK_ROOT}/sdk")
set(brix_private "${CMAKE_SOURCE_DIR}/Source/RendererD3D12/Private")
set(FFX_GPU_PATH "${brix_sdk}/include/FidelityFX/gpu")
set(FFX_SC_EXECUTABLE "${brix_sdk}/tools/binary_store/FidelityFX_SC.exe")
set(FFX_API_BACKEND DX12_X64)
set(FFX_PASS_SHADER_OUTPUT_PATH "${CMAKE_CURRENT_BINARY_DIR}/permutations")
file(MAKE_DIRECTORY "${FFX_PASS_SHADER_OUTPUT_PATH}")
file(READ "${FFX_GPU_PATH}/CMakeCompileShaders.txt" brix_compile_functions)
# The pinned helper aliases the caller's output-variable name and its local
# header list. Keep the name separate so it cannot become a file dependency.
string(REPLACE "\tPERMUTATION_OUTPUTS)" "\tPERMUTATION_OUTPUT_VARIABLE)\n\tset(PERMUTATION_OUTPUTS)" brix_compile_functions "${brix_compile_functions}")
string(REPLACE "set(\${PERMUTATION_OUTPUTS} \${PERMUTATION_OUTPUTS} PARENT_SCOPE)" "set(\${PERMUTATION_OUTPUT_VARIABLE} \${PERMUTATION_OUTPUTS} PARENT_SCOPE)" brix_compile_functions "${brix_compile_functions}")
string(REPLACE "WORKING_DIRECTORY" "JOB_POOL hs_brixelizer_shader_compile\n            WORKING_DIRECTORY" brix_compile_functions "${brix_compile_functions}")
string(REPLACE "DEPENDS \${PASS_SHADER}" "DEPENDS \${PASS_SHADER} \${FFX_SC_EXECUTABLE} \${HS_BRIXELIZER_COMPILER_INPUTS}" brix_compile_functions "${brix_compile_functions}")
# The pinned SC reads its top-level source with std::ifstream and silently
# compiles an empty buffer at MAX_PATH. Keep dependency/identity paths intact;
# only its Windows compiler input uses the native extended-length spelling.
if(WIN32)
    # Its include resolver also constructs temporary paths relative to CWD;
    # the nested build directory can overflow those before normalization.
    string(REPLACE "WORKING_DIRECTORY \${CMAKE_BINARY_DIR}"
        "WORKING_DIRECTORY \${CMAKE_SOURCE_DIR}"
        brix_compile_functions "${brix_compile_functions}")
    set(brix_shader_input_adapter [=[
        file(TO_NATIVE_PATH "${PASS_SHADER}" HS_BRIXELIZER_SHADER_INPUT)
        string(PREPEND HS_BRIXELIZER_SHADER_INPUT "\\\\?\\")
]=])
    set(brix_shader_include_adapter [=[
        file(TO_NATIVE_PATH "${INC}" INC)
        string(PREPEND INC "\\\\?\\")
]=])
    # SC's custom include resolver requires the source and -I paths to use
    # the same Win32 namespace, including when the includes fit MAX_PATH.
    string(REPLACE "foreach(INC \${INCLUDES_ARGS})"
        "foreach(INC \${INCLUDES_ARGS})\n${brix_shader_include_adapter}"
        brix_compile_functions "${brix_compile_functions}")
    string(REPLACE "get_filename_component(PASS_SHADER_TARGET \${PASS_SHADER} NAME_WLE)"
        "get_filename_component(PASS_SHADER_TARGET \${PASS_SHADER} NAME_WLE)\n${brix_shader_input_adapter}"
        brix_compile_functions "${brix_compile_functions}")
    string(REPLACE "-output=\${OUTPUT_PATH} \${PASS_SHADER}"
        "-output=\${OUTPUT_PATH} \"\${HS_BRIXELIZER_SHADER_INPUT}\""
        brix_compile_functions "${brix_compile_functions}")
endif()
set(HS_BRIXELIZER_DEPFILE_ADAPTER "${CMAKE_CURRENT_SOURCE_DIR}/NormalizeShaderDepfile.cmake")
# The pinned SC writes absolute dependency paths without GCC escaping. Repair
# that serialization before CMake imports it, retaining every actual include.
foreach(brix_depfile_header IN ITEMS WAVE32_PERMUTATION_HEADER WAVE64_PERMUTATION_HEADER
        WAVE32_16BIT_PERMUTATION_HEADER WAVE64_16BIT_PERMUTATION_HEADER)
    set(brix_depfile_needle "DEPFILE \${${brix_depfile_header}}.d")
    set(brix_depfile_command "COMMAND \${CMAKE_COMMAND} \"-DHS_DEPFILE=\${${brix_depfile_header}}.d\" \"-DHS_DEPFILE_OUTPUT=\${${brix_depfile_header}}\" \"-DHS_DEPFILE_SOURCE_ROOT=\${HS_BRIXELIZER_SDK_ROOT}\" -P \"\${HS_BRIXELIZER_DEPFILE_ADAPTER}\"\n\t\t\t${brix_depfile_needle}")
    string(REPLACE "${brix_depfile_needle}" "${brix_depfile_command}" brix_compile_functions "${brix_compile_functions}")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/CMakeCompileShaders.txt" CONTENT "${brix_compile_functions}" @ONLY)
include("${CMAKE_CURRENT_BINARY_DIR}/CMakeCompileShaders.txt")
set(HS_BRIXELIZER_COMPILER_INPUTS
    "${brix_sdk}/tools/binary_store/dxcompiler.dll" "${brix_sdk}/tools/binary_store/dxil.dll"
    "${FFX_GPU_PATH}/CMakeCompileShaders.txt" "${CMAKE_SOURCE_DIR}/Tools/bootstrap-brixelizer-sdk.ps1"
    "${HS_BRIXELIZER_SDK_ROOT}/../source-files.sha256" "${HS_BRIXELIZER_DEPFILE_ADAPTER}")
# Limit both outer SC processes and their internal permutation threads. The
# driver documents -num-threads in sdk/tools/ffx_shader_compiler/src/ffx_sc.cpp.
set(CMAKE_JOB_POOL_COMPILE hs_brixelizer_shader_compile)
# Relocate only paths in the official pass lists. All entry points, defines,
# wave/fp16 variants and compiler arguments remain those of the pinned source.
set(BRIXELIZER_API_BASE_ARGS -num-threads=1 -E CS -Wignored-attributes -O3 -no-warnings -T cs_6_6 -DFFX_HLSL=1)
set(BRIXELIZER_GI_API_BASE_ARGS ${BRIXELIZER_API_BASE_ARGS})
foreach(effect IN ITEMS brixelizer brixelizergi)
    if(effect STREQUAL brixelizer)
        set(brix_pass_file CMakeCompileBrixelizerShaders.txt)
    else()
        set(brix_pass_file CMakeCompileBrixelizerGIShaders.txt)
    endif()
    file(READ "${FFX_GPU_PATH}/${effect}/${brix_pass_file}" brix_passes)
    string(REPLACE "\"shaders/" "\"${brix_sdk}/src/backends/dx12/shaders/" brix_passes "${brix_passes}")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${brix_pass_file}" CONTENT "${brix_passes}" @ONLY)
    include("${CMAKE_CURRENT_BINARY_DIR}/${brix_pass_file}")
endforeach()
# The raw pass file appends its outputs, but the GI file only returns its list.
# Own the aggregate here so both effects contribute their actual header paths.
set(FFX_SC_PERMUTATION_OUTPUTS ${BRIXELIZER_PERMUTATION_OUTPUTS} ${BRIXELIZER_GI_PERMUTATION_OUTPUTS})
add_custom_target(hs_brixelizer_permutations DEPENDS ${FFX_SC_PERMUTATION_OUTPUTS})
# Reserve room for the MSVC /sourceDependencies .hsdeps.json sidecar under
# Windows MAX_PATH. This directory scope hashes SDK object paths only.
set(CMAKE_OBJECT_PATH_MAX 180)
file(GLOB brix_shared CONFIGURE_DEPENDS "${brix_sdk}/src/shared/*.cpp")
file(GLOB brix_host CONFIGURE_DEPENDS "${brix_sdk}/src/components/brixelizer/*.cpp" "${brix_sdk}/src/components/brixelizergi/*.cpp")
add_library(hs_brixelizer_shader_tables STATIC ${brix_shared}
    "${brix_sdk}/src/backends/shared/ffx_shader_blobs.cpp"
    "${brix_sdk}/src/backends/shared/blob_accessors/ffx_brixelizer_shaderblobs.cpp"
    "${brix_sdk}/src/backends/shared/blob_accessors/ffx_brixelizergi_shaderblobs.cpp"
    "${brix_private}/renderer_brixelizer_shaders.cpp")
add_dependencies(hs_brixelizer_shader_tables hs_brixelizer_permutations)
target_compile_definitions(hs_brixelizer_shader_tables PRIVATE FFX_BRIXELIZER FFX_BRIXELIZER_GI NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(hs_brixelizer_shader_tables PRIVATE
    "${brix_sdk}/include" "${brix_sdk}/src/components" "${brix_sdk}/src/backends/shared"
    "${brix_sdk}/src/shared" "${FFX_PASS_SHADER_OUTPUT_PATH}")
target_include_directories(hs_brixelizer_shader_tables PUBLIC "${CMAKE_SOURCE_DIR}/Source/RendererD3D12/Public")
add_library(hs_brixelizer_native STATIC ${brix_host} "${brix_private}/renderer_brixelizer_backend.cpp")
target_include_directories(hs_brixelizer_native PRIVATE
    "${brix_sdk}/include" "${brix_sdk}/libs" "${brix_sdk}/src/components"
    "${brix_sdk}/src/shared" "${brix_sdk}/src/backends/shared" "${brix_sdk}/src/backends/dx12")
target_include_directories(hs_brixelizer_native PRIVATE "${brix_sdk}/src/components/brixelizer")
target_compile_definitions(hs_brixelizer_native PRIVATE FFX_BRIXELIZER FFX_BRIXELIZER_GI NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
target_link_libraries(hs_brixelizer_native PRIVATE hs_core hs_brixelizer_shader_tables d3d12 dxgi)
if(MSVC)
    target_compile_options(hs_brixelizer_native PRIVATE /wd4201 /wd4244 /wd4267)
    target_compile_options(hs_brixelizer_shader_tables PRIVATE /wd4201 /wd4244 /wd4267)
endif()
# Consumers get no transitive FidelityFX include root. Set these includes only
# on renderer_brixelizer_gi.cpp (not renderer target, FSR or provider TUs).
set(HS_BRIXELIZER_PRIVATE_INCLUDE "${brix_sdk}/include" CACHE INTERNAL "Brixelizer TU-only headers")
