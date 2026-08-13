file(GLOB_RECURSE gameplay_files
    "${SOURCE_ROOT}/Source/Gameplay/*.hpp"
    "${SOURCE_ROOT}/Source/Gameplay/*.cpp")

foreach(gameplay_file IN LISTS gameplay_files)
    file(READ "${gameplay_file}" contents)
    if(contents MATCHES "#include[ \t]*[<\"]hs/(renderer|runtime|presentation)/")
        message(FATAL_ERROR "gameplay has forbidden presentation/runtime include: ${gameplay_file}")
    endif()
    if(contents MATCHES "D3D12|ID3D12|IDXGI")
        message(FATAL_ERROR "gameplay has forbidden graphics API knowledge: ${gameplay_file}")
    endif()
endforeach()

file(READ "${SOURCE_ROOT}/CMakeLists.txt" cmake_contents)
if(cmake_contents MATCHES "target_link_libraries\\([ \t\r\n]*hs_gameplay[ \t\r\n]+[^\\)]*(hs_presentation|hs_renderer_d3d12|hs_runtime)")
    message(FATAL_ERROR "hs_gameplay links a forbidden higher-level target")
endif()
