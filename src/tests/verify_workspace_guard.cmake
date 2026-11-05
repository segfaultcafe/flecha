foreach(variable IN ITEMS LONGBOW FLECHA_COMPILER FLECHA_STD FIXTURE TEST_ROOT)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/project" "${TEST_ROOT}/tree/nested")
file(COPY "${FIXTURE}/src" DESTINATION "${TEST_ROOT}/project")
file(WRITE "${TEST_ROOT}/project/longbow.toml" [=[
[package]
name = "workspace_guard"
version = "0.1.0"
kind = "program"

[dependencies]
]=])

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "FLECHA_COMPILER=${FLECHA_COMPILER}"
        "FLECHA_STANDARD_LIBRARY=${FLECHA_STD}"
        "${LONGBOW}" build
    WORKING_DIRECTORY "${TEST_ROOT}/project"
    RESULT_VARIABLE buildResult
    OUTPUT_VARIABLE buildOutput
    ERROR_VARIABLE buildError)
if(NOT buildResult EQUAL 0)
    message(FATAL_ERROR
        "workspace_guard build failed: ${buildOutput}${buildError}")
endif()

if(WIN32)
    set(executable "${TEST_ROOT}/project/target/workspace_guard.exe")
else()
    set(executable "${TEST_ROOT}/project/target/workspace_guard")
endif()
if(NOT EXISTS "${executable}")
    message(FATAL_ERROR "workspace_guard executable was not produced")
endif()

function(run_guard expected)
    execute_process(
        COMMAND "${executable}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    string(REPLACE "\r\n" "\n" output "${output}")
    string(REPLACE "\r\n" "\n" errors "${errors}")
    if(NOT result EQUAL expected)
        message(FATAL_ERROR
            "workspace_guard ${ARGN} returned ${result}, expected "
            "${expected}: ${output}${errors}")
    endif()
    set(last_output "${output}" PARENT_SCOPE)
    set(last_errors "${errors}" PARENT_SCOPE)
endfunction()

file(WRITE "${TEST_ROOT}/tree/alpha.txt" "alpha\n")
file(WRITE "${TEST_ROOT}/tree/nested/beta.txt" "beta\n")
file(WRITE "${TEST_ROOT}/tree/space name.txt" "space\n")
set(firstManifest "${TEST_ROOT}/first.manifest")
run_guard(0 snapshot "${TEST_ROOT}/tree" "${firstManifest}")
if(NOT last_output STREQUAL "snapshot: 3 files\n")
    message(FATAL_ERROR "unexpected snapshot output: ${last_output}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "FLECHA_GUARD_ROOT=${TEST_ROOT}/tree"
        "${executable}" verify "${firstManifest}"
    RESULT_VARIABLE environmentVerifyResult
    OUTPUT_VARIABLE environmentVerifyOutput
    ERROR_VARIABLE environmentVerifyError)
string(REPLACE "\r\n" "\n"
    environmentVerifyOutput "${environmentVerifyOutput}")
if(NOT environmentVerifyResult EQUAL 0 OR
   NOT environmentVerifyOutput STREQUAL "verified: 3 files\n")
    message(FATAL_ERROR
        "environment-root verification failed: "
        "${environmentVerifyOutput}${environmentVerifyError}")
endif()

file(SHA256 "${firstManifest}" firstHash)
file(READ "${firstManifest}" firstContents)
if(firstContents MATCHES "${TEST_ROOT}")
    message(FATAL_ERROR "manifest leaked its absolute root")
endif()

file(MAKE_DIRECTORY "${TEST_ROOT}/relocated/nested")
file(COPY "${TEST_ROOT}/tree/alpha.txt"
    DESTINATION "${TEST_ROOT}/relocated")
file(COPY "${TEST_ROOT}/tree/nested/beta.txt"
    DESTINATION "${TEST_ROOT}/relocated/nested")
file(COPY "${TEST_ROOT}/tree/space name.txt"
    DESTINATION "${TEST_ROOT}/relocated")
set(relocatedManifest "${TEST_ROOT}/relocated.manifest")
run_guard(0 snapshot "${TEST_ROOT}/relocated" "${relocatedManifest}")
file(SHA256 "${relocatedManifest}" relocatedHash)
if(NOT firstHash STREQUAL relocatedHash)
    message(FATAL_ERROR "identical relocated trees produced different manifests")
endif()

run_guard(0 diff "${firstManifest}" "${relocatedManifest}")
if(NOT last_output STREQUAL "diff: 0 differences\n")
    message(FATAL_ERROR "unexpected identical diff: ${last_output}")
endif()

file(REMOVE "${TEST_ROOT}/tree/alpha.txt")
file(WRITE "${TEST_ROOT}/tree/gamma.txt" "gamma\n")
file(WRITE "${TEST_ROOT}/tree/nested/beta.txt" "changed\n")
run_guard(5 verify "${TEST_ROOT}/tree" "${firstManifest}")
if(NOT last_errors STREQUAL
   "removed alpha.txt\nadded gamma.txt\nmodified nested/beta.txt\nworkspace_guard: 3 workspace differences\n")
    message(FATAL_ERROR "unexpected mismatch diagnostics: ${last_errors}")
endif()

set(secondManifest "${TEST_ROOT}/second.manifest")
run_guard(0 snapshot "${TEST_ROOT}/tree" "${secondManifest}")
run_guard(0 diff "${firstManifest}" "${secondManifest}")
if(NOT last_output STREQUAL
   "removed alpha.txt\nadded gamma.txt\nmodified nested/beta.txt\ndiff: 3 differences\n")
    message(FATAL_ERROR "unexpected changed diff: ${last_output}")
endif()

# Existing output is preserved byte-for-byte instead of being truncated.
run_guard(3 snapshot "${TEST_ROOT}/relocated" "${firstManifest}")
file(SHA256 "${firstManifest}" preservedHash)
if(NOT firstHash STREQUAL preservedHash)
    message(FATAL_ERROR "failed snapshot changed its existing destination")
endif()

file(WRITE "${TEST_ROOT}/invalid.manifest" "not a manifest\n")
run_guard(4 verify "${TEST_ROOT}/relocated" "${TEST_ROOT}/invalid.manifest")
if(NOT last_errors STREQUAL "workspace_guard: invalid manifest\n")
    message(FATAL_ERROR "unexpected malformed-manifest error: ${last_errors}")
endif()

run_guard(3 verify "${TEST_ROOT}/missing" "${firstManifest}")

if(UNIX)
    file(CREATE_LINK "${TEST_ROOT}/relocated/alpha.txt"
        "${TEST_ROOT}/relocated/link.txt" SYMBOLIC RESULT linkResult)
    if(NOT linkResult STREQUAL "0")
        message(FATAL_ERROR "could not create symlink fixture: ${linkResult}")
    endif()
    set(rejectedManifest "${TEST_ROOT}/rejected.manifest")
    run_guard(6 snapshot "${TEST_ROOT}/relocated" "${rejectedManifest}")
    if(EXISTS "${rejectedManifest}")
        message(FATAL_ERROR "failed scan left a partial manifest")
    endif()
    file(REMOVE "${TEST_ROOT}/relocated/link.txt")
endif()

run_guard(0 run "${TEST_ROOT}/relocated" "${firstManifest}" --
    "${CMAKE_COMMAND}" -E echo "guard-child")
if(NOT last_output STREQUAL "guard-child\n")
    message(FATAL_ERROR "child output was not inherited: ${last_output}")
endif()
run_guard(1 run "${TEST_ROOT}/relocated" "${firstManifest}" --
    "${CMAKE_COMMAND}" -E false)

