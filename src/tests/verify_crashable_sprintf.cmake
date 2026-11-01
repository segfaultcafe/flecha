foreach(variable IN ITEMS FLECHA_COMPILER FLECHA_SOURCE FLECHA_OUTPUT FLECHA_STD)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${FLECHA_SOURCE}" -o "${FLECHA_OUTPUT}"
        --lib "std=${FLECHA_STD}"
    RESULT_VARIABLE compileResult
    ERROR_VARIABLE compileError
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR "sprintf! compilation failed: ${compileError}")
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    OUTPUT_VARIABLE actualOutput
    ERROR_VARIABLE actualError
)
if(NOT runResult EQUAL 0)
    message(FATAL_ERROR
        "sprintf! program returned ${runResult}\nstderr:\n${actualError}")
endif()
if(NOT actualOutput STREQUAL "value 42")
    message(FATAL_ERROR "sprintf! output mismatch: '${actualOutput}'")
endif()
if(NOT actualError STREQUAL "")
    message(FATAL_ERROR "sprintf! wrote unexpected stderr: ${actualError}")
endif()
