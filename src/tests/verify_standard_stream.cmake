execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${FLECHA_SOURCE}" -o "${FLECHA_OUTPUT}"
        --lib "std=${FLECHA_STD}"
    RESULT_VARIABLE compileResult
    ERROR_VARIABLE compileError
    TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR "standard-stream compilation failed: ${compileError}")
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    OUTPUT_VARIABLE actualOutput
    ERROR_VARIABLE actualError
    TIMEOUT 10
)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR
        "standard-stream program returned ${runResult}, expected 42"
    )
endif()

set(expectedOutput
    "builtin output|direct output|buffered|print 42|line|\nformatted true {}|"
)
if(NOT actualOutput STREQUAL expectedOutput)
    message(FATAL_ERROR
        "standard output mismatch\nexpected: ${expectedOutput}\nactual: ${actualOutput}"
    )
endif()

set(expectedError
    "builtin error|direct error|buffered|eprint 42|eline|\neformatted false {}|"
)
if(NOT actualError STREQUAL expectedError)
    message(FATAL_ERROR
        "standard error mismatch\nexpected: ${expectedError}\nactual: ${actualError}"
    )
endif()
