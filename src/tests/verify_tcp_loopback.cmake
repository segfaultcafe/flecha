execute_process(
    COMMAND "${FLECHA_COMPILER}" --emit-object "${FLECHA_SOURCE}"
        --lib "std=${FLECHA_STD}" -o "${FLECHA_OUTPUT}.o"
    RESULT_VARIABLE compileResult
    ERROR_VARIABLE compileError
    TIMEOUT 60
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR
        "TCP loopback fixture compilation failed: ${compileError}"
    )
endif()

set(linkFlags)
if(FLECHA_SANITIZERS)
    list(APPEND linkFlags -fsanitize=address,undefined)
endif()
if(UNIX OR MINGW)
    list(APPEND linkFlags -pthread)
endif()
if(WIN32)
    list(APPEND linkFlags -lbcrypt -lws2_32)
endif()

execute_process(
    COMMAND "${FLECHA_LINKER}" "${FLECHA_HARNESS}" "${FLECHA_OUTPUT}.o"
        "-I${FLECHA_RUNTIME_INCLUDE}" "${FLECHA_RUNTIME}" ${linkFlags}
        -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE linkResult
    ERROR_VARIABLE linkError
    TIMEOUT 60
)
if(NOT linkResult EQUAL 0)
    message(FATAL_ERROR "TCP loopback fixture linking failed: ${linkError}")
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    ERROR_VARIABLE runError
    TIMEOUT 30
)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR
        "TCP loopback fixture returned ${runResult}, expected 42: ${runError}"
    )
endif()
