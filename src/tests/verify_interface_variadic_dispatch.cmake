execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${FLECHA_SOURCE}" -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE compileResult
    OUTPUT_VARIABLE compileOutput
    ERROR_VARIABLE compileError
    TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR
        "variadic interface compilation failed: ${compileOutput}${compileError}"
    )
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    OUTPUT_VARIABLE runOutput
    ERROR_VARIABLE runError
    TIMEOUT 10
)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR
        "variadic interface dispatch returned ${runResult}, expected 42: ${runError}"
    )
endif()
if(NOT runOutput STREQUAL "" OR NOT runError STREQUAL "")
    message(FATAL_ERROR
        "variadic interface dispatch emitted unexpected output: ${runOutput}${runError}"
    )
endif()
