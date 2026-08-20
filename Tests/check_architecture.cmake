function(hs_check_module module forbidden_include_regex)
    file(GLOB_RECURSE module_files
        "${SOURCE_ROOT}/Source/${module}/*.h"
        "${SOURCE_ROOT}/Source/${module}/*.hpp"
        "${SOURCE_ROOT}/Source/${module}/*.cpp"
        "${SOURCE_ROOT}/Source/${module}/*.inl")

    foreach(module_file IN LISTS module_files)
        file(READ "${module_file}" contents)
        if(contents MATCHES "#include[ \t]*[<\"]hs/(${forbidden_include_regex})/")
            message(FATAL_ERROR "${module} has a forbidden module include: ${module_file}")
        endif()
        if(contents MATCHES "#include[^\r\n]*(Source/)?(Core|Jobs|GameDomain|GameRules|Gameplay|Presentation|RendererD3D12|Runtime)/Private")
            message(FATAL_ERROR "production code includes another module's Private directory: ${module_file}")
        endif()
    endforeach()
endfunction()

hs_check_module(Core "jobs|game_domain|game_rules|gameplay|presentation|renderer|runtime")
hs_check_module(Jobs "game_domain|game_rules|gameplay|presentation|renderer|runtime")
hs_check_module(GameDomain "jobs|game_rules|gameplay|presentation|renderer|runtime")
hs_check_module(GameRules "jobs|gameplay|presentation|renderer|runtime")
hs_check_module(Gameplay "jobs|presentation|renderer|runtime")
hs_check_module(Presentation "jobs|game_rules|gameplay|renderer|runtime")
hs_check_module(RendererD3D12 "jobs|game_domain|game_rules|gameplay|presentation|runtime")

file(GLOB_RECURSE gameplay_files
    "${SOURCE_ROOT}/Source/Gameplay/*.h"
    "${SOURCE_ROOT}/Source/Gameplay/*.hpp"
    "${SOURCE_ROOT}/Source/Gameplay/*.cpp"
    "${SOURCE_ROOT}/Source/Gameplay/*.inl")
foreach(gameplay_file IN LISTS gameplay_files)
    file(READ "${gameplay_file}" contents)
    if(contents MATCHES "D3D12|ID3D12|IDXGI")
        message(FATAL_ERROR "Gameplay has forbidden graphics API knowledge: ${gameplay_file}")
    endif()
endforeach()

file(READ "${SOURCE_ROOT}/CMakeLists.txt" cmake_contents)
function(hs_reject_target_links target)
    foreach(forbidden IN LISTS ARGN)
        if(cmake_contents MATCHES "target_link_libraries\\([ \t\r\n]*${target}[ \t\r\n]+[^\\)]*${forbidden}([ \t\r\n]|\\))")
            message(FATAL_ERROR "${target} links forbidden target ${forbidden}")
        endif()
    endforeach()
endfunction()

hs_reject_target_links(hs_core hs_jobs hs_game_domain hs_game_rules hs_gameplay hs_presentation hs_renderer_d3d12 hs_runtime)
hs_reject_target_links(hs_jobs hs_game_domain hs_game_rules hs_gameplay hs_presentation hs_renderer_d3d12 hs_runtime)
hs_reject_target_links(hs_game_domain hs_jobs hs_game_rules hs_gameplay hs_presentation hs_renderer_d3d12 hs_runtime)
hs_reject_target_links(hs_game_rules hs_jobs hs_gameplay hs_presentation hs_renderer_d3d12 hs_runtime)
hs_reject_target_links(hs_gameplay hs_jobs hs_presentation hs_renderer_d3d12 hs_runtime)
hs_reject_target_links(hs_presentation hs_jobs hs_game_rules hs_gameplay hs_renderer_d3d12 hs_runtime)
hs_reject_target_links(hs_renderer_d3d12 hs_jobs hs_game_domain hs_game_rules hs_gameplay hs_presentation hs_runtime)
