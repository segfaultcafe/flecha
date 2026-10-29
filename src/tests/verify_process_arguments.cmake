execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${FLECHA_SOURCE}"
        -o "${FLECHA_OUTPUT}" --lib "std=${FLECHA_STD}"
    RESULT_VARIABLE compileResult
    ERROR_VARIABLE compileError
    TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR "process-argument compilation failed: ${compileError}")
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}" "alpha" "two words" "" "--flag=value"
    RESULT_VARIABLE runResult
    ERROR_VARIABLE runError
    TIMEOUT 10
)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR
        "process-argument program returned ${runResult}, expected 42: ${runError}"
    )
endif()
