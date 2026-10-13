# Git for Windows can materialize a patch with CRLF under core.autocrlf.
# Repair only a byte-identical known pre/postimage after CRLF normalization;
# all other dependency modifications still fail closed.
function(hs_taskflow_normalize_known path before after)
    file(SHA256 "${path}" digest)
    if(digest STREQUAL before OR digest STREQUAL after)
        return()
    endif()
    file(READ "${path}" text)
    string(REPLACE "\r\n" "\n" normalized "${text}")
    string(SHA256 normalized_digest "${normalized}")
    if(NOT normalized_digest STREQUAL before AND NOT normalized_digest STREQUAL after)
        message(FATAL_ERROR "Unexpected Taskflow source: ${path}")
    endif()
    file(WRITE "${path}" "${normalized}")
endfunction()

# Only the exact pinned preimage or this exact patched image is accepted.
function(hs_patch_taskflow source)
    find_package(Git REQUIRED)
    set(needs_patch FALSE)
    hs_taskflow_normalize_known("${source}/taskflow/core/async.hpp"
        "90754848e9488822e97c5686c1a1693d96a923b7b86080f55287b824841892c0"
        "a6a724648ec6fcbbb2ea4fd0336110b59906e876883150026114ecd54a294bf3")
    file(SHA256 "${source}/taskflow/core/async.hpp" digest)
    if(digest STREQUAL "90754848e9488822e97c5686c1a1693d96a923b7b86080f55287b824841892c0")
        set(needs_patch TRUE)
    elseif(NOT digest STREQUAL "a6a724648ec6fcbbb2ea4fd0336110b59906e876883150026114ecd54a294bf3")
        message(FATAL_ERROR "Unexpected Taskflow source: taskflow/core/async.hpp")
    endif()
    hs_taskflow_normalize_known("${source}/taskflow/core/wsq.hpp"
        "51bfb485bf5988680f69e18e68c247609a94ab84f6db73701a3b8a53d13df104"
        "da8b2c80580996b2c480726b319873437a826c11c5abba5fda3995fdef6f657c")
    file(SHA256 "${source}/taskflow/core/wsq.hpp" digest)
    if(digest STREQUAL "51bfb485bf5988680f69e18e68c247609a94ab84f6db73701a3b8a53d13df104")
        set(needs_patch TRUE)
    elseif(NOT digest STREQUAL "da8b2c80580996b2c480726b319873437a826c11c5abba5fda3995fdef6f657c")
        message(FATAL_ERROR "Unexpected Taskflow source: taskflow/core/wsq.hpp")
    endif()
    if(needs_patch)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -c core.autocrlf=false -c core.eol=lf apply --check
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/taskflow-async-admission.patch"
            WORKING_DIRECTORY "${source}" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -c core.autocrlf=false -c core.eol=lf apply
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/taskflow-async-admission.patch"
            WORKING_DIRECTORY "${source}" COMMAND_ERROR_IS_FATAL ANY)
    endif()
    file(SHA256 "${source}/taskflow/core/async.hpp" digest)
    if(NOT digest STREQUAL "a6a724648ec6fcbbb2ea4fd0336110b59906e876883150026114ecd54a294bf3")
        message(FATAL_ERROR "Taskflow patch verification failed: taskflow/core/async.hpp")
    endif()
    file(SHA256 "${source}/taskflow/core/wsq.hpp" digest)
    if(NOT digest STREQUAL "da8b2c80580996b2c480726b319873437a826c11c5abba5fda3995fdef6f657c")
        message(FATAL_ERROR "Taskflow patch verification failed: taskflow/core/wsq.hpp")
    endif()
endfunction()
