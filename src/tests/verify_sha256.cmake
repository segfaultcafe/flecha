execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${FLECHA_SOURCE}"
        -o "${FLECHA_OUTPUT}" --lib "std=${FLECHA_STD}"
    RESULT_VARIABLE compileResult
    ERROR_VARIABLE compileError
    TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR "SHA-256 fixture compilation failed: ${compileError}")
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    ERROR_VARIABLE runError
    TIMEOUT 30
)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR
        "SHA-256 fixture returned ${runResult}, expected 42: ${runError}"
    )
endif()
