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

foreach(layer IN ITEMS GameRules GameDomain)
    file(GLOB_RECURSE layer_files
        "${SOURCE_ROOT}/Source/${layer}/*.hpp"
        "${SOURCE_ROOT}/Source/${layer}/*.cpp")
    foreach(layer_file IN LISTS layer_files)
        file(READ "${layer_file}" contents)
        if(contents MATCHES "#include[ \t]*[<\"]hs/(renderer|runtime|presentation)/")
            message(FATAL_ERROR "${layer} has forbidden higher-level include: ${layer_file}")
        endif()
    endforeach()
endforeach()

file(GLOB_RECURSE presentation_files
    "${SOURCE_ROOT}/Source/Presentation/*.hpp"
    "${SOURCE_ROOT}/Source/Presentation/*.cpp")
foreach(presentation_file IN LISTS presentation_files)
    file(READ "${presentation_file}" contents)
    if(contents MATCHES "Source/Gameplay/Private|#include[ \t]*[<\"]hs/gameplay/")
        message(FATAL_ERROR "presentation depends on gameplay implementation: ${presentation_file}")
    endif()
    if(contents MATCHES "#include[ \t]*[<\"]hs/game_rules/")
        message(FATAL_ERROR "presentation depends on simulation rules: ${presentation_file}")
    endif()
endforeach()

file(READ "${SOURCE_ROOT}/CMakeLists.txt" cmake_contents)
if(cmake_contents MATCHES "target_link_libraries\\([ \t\r\n]*hs_gameplay[ \t\r\n]+[^\\)]*(hs_presentation|hs_renderer_d3d12|hs_runtime)")
    message(FATAL_ERROR "hs_gameplay links a forbidden higher-level target")
endif()
if(cmake_contents MATCHES "target_link_libraries\\([ \t\r\n]*hs_presentation[ \t\r\n]+[^\\)]*hs_gameplay")
    message(FATAL_ERROR "hs_presentation links gameplay")
endif()
if(cmake_contents MATCHES "target_link_libraries\\([ \t\r\n]*hs_presentation[ \t\r\n]+[^\\)]*hs_game_rules")
    message(FATAL_ERROR "hs_presentation links simulation rules")
endif()
