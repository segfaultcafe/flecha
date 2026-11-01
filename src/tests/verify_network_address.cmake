execute_process(
    COMMAND "${FLECHA_COMPILER}" --emit-object "${FLECHA_SOURCE}"
        --lib "std=${FLECHA_STD}" -o "${FLECHA_OUTPUT}.o"
    RESULT_VARIABLE compileResult
    ERROR_VARIABLE compileError
    TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR
        "network address fixture compilation failed: ${compileError}"
    )
endif()

set(sanitizerFlags)
if(FLECHA_SANITIZERS)
    list(APPEND sanitizerFlags -fsanitize=address,undefined)
endif()
execute_process(
    COMMAND "${FLECHA_LINKER}" "${FLECHA_OUTPUT}.o" "${FLECHA_RUNTIME}"
        ${sanitizerFlags} -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE linkResult
    ERROR_VARIABLE linkError
    TIMEOUT 30
)
if(NOT linkResult EQUAL 0)
    message(FATAL_ERROR
        "network address fixture linking failed: ${linkError}"
    )
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    ERROR_VARIABLE runError
    TIMEOUT 10
)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR
        "network address fixture returned ${runResult}, expected 42: ${runError}"
    )
endif()
