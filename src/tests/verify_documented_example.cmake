foreach(required FLECHA_COMPILER PROJECT_ROOT FLECHA_OUTPUT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

set(documentedSource "examples/hello.fl")

execute_process(
    COMMAND "${FLECHA_COMPILER}" check "${documentedSource}"
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    RESULT_VARIABLE checkResult
    OUTPUT_VARIABLE checkOutput
    ERROR_VARIABLE checkError
)
if(NOT checkResult EQUAL 0)
    message(FATAL_ERROR
        "documented example check failed (${checkResult}):\n"
        "${checkOutput}${checkError}")
endif()

file(REMOVE "${FLECHA_OUTPUT}")
execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${documentedSource}"
        -o "${FLECHA_OUTPUT}"
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    RESULT_VARIABLE buildResult
    OUTPUT_VARIABLE buildOutput
    ERROR_VARIABLE buildError
)
if(NOT buildResult EQUAL 0)
    message(FATAL_ERROR
        "documented example build failed (${buildResult}):\n"
        "${buildOutput}${buildError}")
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    OUTPUT_VARIABLE runOutput
    ERROR_VARIABLE runError
)
if(NOT runResult EQUAL 0)
    message(FATAL_ERROR
        "documented example execution failed (${runResult}):\n"
        "${runOutput}${runError}")
endif()

string(REPLACE "\r\n" "\n" runOutput "${runOutput}")
set(expectedOutput "Flecha v0.3: 42\n")
if(NOT runOutput STREQUAL expectedOutput)
    message(FATAL_ERROR
        "documented example output mismatch:\n"
        "expected: [${expectedOutput}]\n"
        "actual:   [${runOutput}]")
endif()
