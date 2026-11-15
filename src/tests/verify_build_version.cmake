foreach(required SOURCE_ROOT TEST_ROOT GIT_EXECUTABLE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/source")
file(WRITE "${TEST_ROOT}/source/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.20)\n"
    "project(VersionFixture VERSION 0.3.3 LANGUAGES NONE)\n"
    "include(\"${SOURCE_ROOT}/cmake/flecha_version.cmake\")\n"
    "file(WRITE \"\${CMAKE_BINARY_DIR}/version.txt\" \"\${FLECHA_BUILD_VERSION}\")\n")
file(WRITE "${TEST_ROOT}/source/tracked.txt" "fixture\n")

function(run_git)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" ${ARGN}
        WORKING_DIRECTORY "${TEST_ROOT}/source"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed: ${output}${error}")
    endif()
endfunction()

run_git(init)
run_git(config user.name "Flecha Tests")
run_git(config user.email "tests@flecha.invalid")
run_git(add CMakeLists.txt tracked.txt)
run_git(commit -m initial)
run_git(tag v0.3.2)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${TEST_ROOT}/source"
        -B "${TEST_ROOT}/build"
    RESULT_VARIABLE configureResult
    OUTPUT_VARIABLE configureOutput
    ERROR_VARIABLE configureError
)
if(NOT configureResult EQUAL 0)
    message(FATAL_ERROR
        "version fixture configuration failed: ${configureOutput}${configureError}")
endif()
file(READ "${TEST_ROOT}/build/version.txt" version)
if(NOT version MATCHES "^0[.]3[.]3-dev-g[0-9a-f]+$")
    message(FATAL_ERROR "unexpected tagless development version: ${version}")
endif()

file(APPEND "${TEST_ROOT}/source/tracked.txt" "dirty\n")
file(REMOVE_RECURSE "${TEST_ROOT}/build")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${TEST_ROOT}/source"
        -B "${TEST_ROOT}/build"
    RESULT_VARIABLE dirtyConfigureResult
    OUTPUT_VARIABLE dirtyConfigureOutput
    ERROR_VARIABLE dirtyConfigureError
)
if(NOT dirtyConfigureResult EQUAL 0)
    message(FATAL_ERROR
        "dirty version fixture configuration failed: "
        "${dirtyConfigureOutput}${dirtyConfigureError}")
endif()
file(READ "${TEST_ROOT}/build/version.txt" dirtyVersion)
if(NOT dirtyVersion MATCHES "^0[.]3[.]3-dev-g[0-9a-f]+-dirty$")
    message(FATAL_ERROR "unexpected dirty development version: ${dirtyVersion}")
endif()
