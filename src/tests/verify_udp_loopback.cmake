execute_process(
    COMMAND "${FLECHA_COMPILER}" --emit-object "${FLECHA_SOURCE}"
        --lib "std=${FLECHA_STD}" -o "${FLECHA_OUTPUT}.o"
    RESULT_VARIABLE compileResult
    ERROR_VARIABLE compileError
    TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR
        "UDP loopback fixture compilation failed: ${compileError}"
    )
endif()

set(linkFlags)
if(FLECHA_SANITIZERS)
    list(APPEND linkFlags -fsanitize=address,undefined)
endif()
if(WIN32)
    list(APPEND linkFlags -lbcrypt -lws2_32)
endif()

execute_process(
    COMMAND "${FLECHA_LINKER}" "${FLECHA_OUTPUT}.o" "${FLECHA_RUNTIME}"
        ${linkFlags} -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE linkResult
    ERROR_VARIABLE linkError
    TIMEOUT 30
)
if(NOT linkResult EQUAL 0)
    message(FATAL_ERROR "UDP loopback fixture linking failed: ${linkError}")
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    ERROR_VARIABLE runError
    TIMEOUT 10
)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR
        "UDP loopback fixture returned ${runResult}, expected 42: ${runError}"
    )
endif()
