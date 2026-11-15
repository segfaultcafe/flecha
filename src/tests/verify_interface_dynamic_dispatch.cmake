execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${FLECHA_SOURCE}" -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE compileResult
    ERROR_VARIABLE compileError
    TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR "native compilation failed: ${compileError}")
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
        "dynamic dispatch returned ${runResult}, expected 42: ${actualError}"
    )
endif()
if(NOT actualOutput STREQUAL "" OR NOT actualError STREQUAL "")
    message(FATAL_ERROR
        "dynamic dispatch emitted unexpected output: ${actualOutput}${actualError}"
    )
endif()
