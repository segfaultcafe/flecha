execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${FLECHA_SOURCE}/main.fl"
        --root "${FLECHA_SOURCE}" -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE compileResult
    OUTPUT_VARIABLE compileOutput
    ERROR_VARIABLE compileError
    TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR
        "interface completion compilation failed: ${compileOutput}${compileError}"
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
        "interface completion returned ${runResult}, expected 42: ${runError}"
    )
endif()
if(NOT runOutput STREQUAL "" OR NOT runError STREQUAL "")
    message(FATAL_ERROR
        "interface completion emitted unexpected output: ${runOutput}${runError}"
    )
endif()
