# Describe each producer's target closure at generation time. The
# manifest itself is written after the producer has compiled, so its source
# inputs come from completed compiler dependency records.
function(_hs_asset_recipe_glob_path output path)
    # CMake GLOB treats brackets in literal directory names as character
    # classes. Encode them before composing the SDK shader lookup pattern.
    string(REPLACE "[" "@_HS_GLOB_OPEN_@" escaped "${path}")
    string(REPLACE "]" "@_HS_GLOB_CLOSE_@" escaped "${escaped}")
    string(REPLACE "@_HS_GLOB_OPEN_@" "[[]" escaped "${escaped}")
    string(REPLACE "@_HS_GLOB_CLOSE_@" "[]]" escaped "${escaped}")
    set(${output} "${escaped}" PARENT_SCOPE)
endfunction()

function(_hs_asset_recipe_abs_path output path base_directory)
    set(value "${path}")
    if(NOT IS_ABSOLUTE "${value}")
        cmake_path(ABSOLUTE_PATH value BASE_DIRECTORY "${base_directory}" NORMALIZE)
    else()
        cmake_path(NORMAL_PATH value)
    endif()
    string(REPLACE "\\" "/" value "${value}")
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(_hs_asset_recipe_is_prefix output root path)
    # Keep the root as the path variable expected by cmake_path while passing
    # the candidate as its expanded path argument.
    set(root_value "${root}")
    set(path_value "${path}")
    string(REPLACE "\\" "/" root_value "${root_value}")
    string(REPLACE "\\" "/" path_value "${path_value}")
    cmake_path(IS_PREFIX root_value "${path_value}" NORMALIZE result)
    set(${output} "${result}" PARENT_SCOPE)
endfunction()

function(_hs_asset_recipe_is_sdk_source output path)
    set(sdk_root "${CMAKE_BINARY_DIR}/_deps")
    _hs_asset_recipe_is_prefix(in_sdk_root "${sdk_root}" "${path}")
    if(in_sdk_root)
        file(RELATIVE_PATH relative "${sdk_root}" "${path}")
        if(relative MATCHES "^[^/]+-src(/|$)")
            set(result TRUE)
        else()
            set(result FALSE)
        endif()
    else()
        set(result FALSE)
    endif()
    set(${output} "${result}" PARENT_SCOPE)
endfunction()

function(hs_generate_asset_recipe_identities)
    get_property(producers GLOBAL PROPERTY HS_ASSET_RECIPE_PRODUCERS)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/RecipeIdentity")
    foreach(producer IN LISTS producers)
        set(pending "${producer}")
        set(visited)
        set(authored_inputs)
        if(HS_MSVC_INCLUDE_PROJECTOR)
            list(APPEND authored_inputs "${HS_MSVC_INCLUDE_PROJECTOR}")
        endif()
        set(sdk_binaries)
        set(external_header_roots)
        string(TOUPPER "${CMAKE_BUILD_TYPE}" configuration)
        set(build_options)
        string(APPEND build_options "global:CMAKE_CXX_FLAGS:${CMAKE_CXX_FLAGS}")
        string(APPEND build_options "\nglobal:CMAKE_CXX_FLAGS_${configuration}:${CMAKE_CXX_FLAGS_${configuration}}")
        string(APPEND build_options "\nglobal:CMAKE_EXE_LINKER_FLAGS:${CMAKE_EXE_LINKER_FLAGS}")
        string(APPEND build_options "\nglobal:CMAKE_EXE_LINKER_FLAGS_${configuration}:${CMAKE_EXE_LINKER_FLAGS_${configuration}}")

        # Walk the raw target graph to discover its exact linked target
        # membership. Property values that depend on configuration are emitted
        # below through TARGET_GENEX_EVAL at generation time.
        while(pending)
            list(POP_FRONT pending dependency)
            if(NOT TARGET "${dependency}" OR dependency IN_LIST visited)
                continue()
            endif()
            list(APPEND visited "${dependency}")
            get_target_property(alias "${dependency}" ALIASED_TARGET)
            if(alias AND NOT alias MATCHES "-NOTFOUND$")
                list(APPEND pending "${alias}")
                continue()
            endif()

            get_target_property(source_dir "${dependency}" SOURCE_DIR)
            if(NOT source_dir OR source_dir MATCHES "-NOTFOUND$")
                set(source_dir "${CMAKE_SOURCE_DIR}")
            endif()
            _hs_asset_recipe_is_prefix(source_dir_in_build "${CMAKE_BINARY_DIR}" "${source_dir}")
            set(source_dir_dependency FALSE)
            if(source_dir_in_build)
                file(RELATIVE_PATH source_dir_build_relative "${CMAKE_BINARY_DIR}" "${source_dir}")
                if(source_dir_build_relative MATCHES "^_deps/[^/]+-src(/|$)")
                    set(source_dir_dependency TRUE)
                endif()
            endif()
            if(source_dir_dependency)
                # DirectXTex and similar SDKs compile authored shader sources
                # into generated .inc files. Keep those authored inputs while
                # taking the generated dependency itself from Ninja.
                _hs_asset_recipe_glob_path(glob_source_dir "${source_dir}")
                file(GLOB_RECURSE shader_sources CONFIGURE_DEPENDS
                    "${glob_source_dir}/*.hlsl" "${glob_source_dir}/*.hlsli"
                    "${glob_source_dir}/CompileShaders.cmd"
                    "${glob_source_dir}/*/CompileShaders.cmd")
                list(APPEND authored_inputs ${shader_sources})
                if(dependency STREQUAL "DirectXTex" AND EXISTS "${DIRECTX_FXC_TOOL}")
                    list(APPEND sdk_binaries "${DIRECTX_FXC_TOOL}")
                endif()
            endif()

            foreach(property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
                get_target_property(libraries "${dependency}" ${property})
                if(NOT libraries OR libraries MATCHES "-NOTFOUND$")
                    continue()
                endif()
                foreach(library IN LISTS libraries)
                    if(library STREQUAL "debug" OR library STREQUAL "optimized" OR library STREQUAL "general")
                        continue()
                    endif()
                    if(library MATCHES "^\\$<LINK_ONLY:([^>]+)>$")
                        set(library "${CMAKE_MATCH_1}")
                    endif()
                    if(TARGET "${library}")
                        list(APPEND pending "${library}")
                    elseif(EXISTS "${library}" AND NOT IS_DIRECTORY "${library}")
                        list(APPEND sdk_binaries "${library}")
                    endif()
                endforeach()
            endforeach()

            get_target_property(imported "${dependency}" IMPORTED)
            if(imported AND NOT imported MATCHES "-NOTFOUND$")
                set(imported_location)
                if(configuration)
                    get_target_property(imported_location "${dependency}" "IMPORTED_LOCATION_${configuration}")
                endif()
                if(NOT imported_location OR imported_location MATCHES "-NOTFOUND$")
                    get_target_property(imported_location "${dependency}" IMPORTED_LOCATION)
                endif()
                if(imported_location AND EXISTS "${imported_location}")
                    list(APPEND sdk_binaries "${imported_location}")
                endif()
                set(imported_implib)
                if(configuration)
                    get_target_property(imported_implib "${dependency}" "IMPORTED_IMPLIB_${configuration}")
                endif()
                if(NOT imported_implib OR imported_implib MATCHES "-NOTFOUND$")
                    get_target_property(imported_implib "${dependency}" IMPORTED_IMPLIB)
                endif()
                if(imported_implib AND EXISTS "${imported_implib}")
                    list(APPEND sdk_binaries "${imported_implib}")
                endif()
            endif()
        endwhile()

        list(REMOVE_DUPLICATES visited)
        list(REMOVE_DUPLICATES authored_inputs)
        list(REMOVE_DUPLICATES sdk_binaries)
        list(REMOVE_DUPLICATES external_header_roots)
        list(SORT visited)
        list(SORT authored_inputs)
        list(SORT sdk_binaries)
        list(SORT external_header_roots)
        string(REPLACE "\\" "/" source_root_for_descriptor "${CMAKE_SOURCE_DIR}")
        string(REPLACE "\\" "/" binary_root_for_descriptor "${CMAKE_BINARY_DIR}")
        string(REPLACE "\\" "/" ninja_for_descriptor "${CMAKE_MAKE_PROGRAM}")
        string(CONCAT descriptor_content
            "set(HS_RECIPE_PRODUCER [=[${producer}]=])\n"
            "set(HS_RECIPE_SOURCE_ROOT [=[${source_root_for_descriptor}]=])\n"
            "set(HS_RECIPE_BINARY_ROOT [=[${binary_root_for_descriptor}]=])\n"
            "set(HS_RECIPE_NINJA [=[${ninja_for_descriptor}]=])\n"
            "set(HS_RECIPE_COMPILE_DATABASE [=[${binary_root_for_descriptor}/compile_commands.json]=])\n"
            "set(HS_RECIPE_CONFIGURATION [=[${CMAKE_BUILD_TYPE}]=])\n"
            "set(HS_RECIPE_COMPILER [=[${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}]=])\n"
            "set(HS_RECIPE_POINTER_SIZE [=[${CMAKE_SIZEOF_VOID_P}]=])\n"
            "set(HS_RECIPE_PRODUCER_OUTPUT [=[$<TARGET_FILE:${producer}>]=])\n")
        string(APPEND descriptor_content "set(HS_RECIPE_BUILD_OPTIONS [=[${build_options}]=])\n")
        foreach(value IN LISTS authored_inputs)
            string(APPEND descriptor_content "list(APPEND HS_RECIPE_AUTHORED_INPUTS [=[${value}]=])\n")
        endforeach()
        foreach(value IN LISTS sdk_binaries)
            string(APPEND descriptor_content "list(APPEND HS_RECIPE_SDK_BINARIES [=[${value}]=])\n")
        endforeach()
        foreach(value IN LISTS external_header_roots)
            string(APPEND descriptor_content "list(APPEND HS_RECIPE_EXTERNAL_HEADER_ROOTS [=[${value}]=])\n")
        endforeach()

        set(target_index 0)
        foreach(dependency IN LISTS visited)
            string(APPEND descriptor_content
                "list(APPEND HS_RECIPE_TARGET_NAMES [=[${dependency}]=])\n"
                "set(HS_RECIPE_TARGET_SOURCE_DIR_${target_index} [=[$<TARGET_PROPERTY:${dependency},SOURCE_DIR>]=])\n"
                "set(HS_RECIPE_TARGET_SOURCES_${target_index} [=[$<TARGET_GENEX_EVAL:${dependency},$<TARGET_PROPERTY:${dependency},SOURCES>>]=])\n")
            math(EXPR target_index "${target_index} + 1")
        endforeach()
        string(APPEND descriptor_content "set(HS_RECIPE_TARGET_COUNT ${target_index})\n")

        set(descriptor "${CMAKE_BINARY_DIR}/RecipeIdentity/${producer}.descriptor.cmake")
        # file(GENERATE) is the supported generation-time evaluator for target
        # properties. The recipe manifest remains a build-time output and is
        # never populated from configure state or a warm dependency snapshot.
        file(GENERATE OUTPUT "${descriptor}" CONTENT "${descriptor_content}" TARGET "${producer}")

        get_target_property(manifest "${producer}" HS_ASSET_RECIPE_MANIFEST_PATH)
        if(NOT manifest OR manifest MATCHES "-NOTFOUND$")
            set(manifest "${CMAKE_BINARY_DIR}/RecipeIdentity/${producer}.json")
        endif()
        add_custom_command(
            OUTPUT "${manifest}"
            COMMAND "${CMAKE_COMMAND}"
                "-DHS_RECIPE_DESCRIPTOR=${descriptor}"
                "-DHS_RECIPE_MANIFEST=${manifest}"
                -P "${CMAKE_SOURCE_DIR}/cmake/AssetRecipeIdentityBuild.cmake"
            DEPENDS "${producer}" "${descriptor}"
                "${CMAKE_SOURCE_DIR}/cmake/AssetRecipeIdentityBuild.cmake"
                ${HS_MSVC_INCLUDE_PROJECTOR}
            VERBATIM)
        get_target_property(prep_target "${producer}" HS_ASSET_RECIPE_PREP_TARGET)
        if(NOT prep_target OR prep_target MATCHES "-NOTFOUND$")
            set(prep_target "${producer}_recipe_identity")
            set_property(TARGET "${producer}" PROPERTY HS_ASSET_RECIPE_PREP_TARGET
                "${prep_target}")
        endif()
        # The manifest rule depends on the producer executable, while the
        # producer has no edge back to the manifest. Keep preparation separate
        # so post-link identity generation cannot form a cycle.
        if(NOT TARGET "${prep_target}")
            add_custom_target("${prep_target}" DEPENDS "${manifest}")
        endif()
        set_property(TARGET "${producer}" PROPERTY HS_ASSET_RECIPE_MANIFEST_PATH "${manifest}")
    endforeach()
endfunction()
