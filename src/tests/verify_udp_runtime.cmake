execute_process(
    COMMAND "${FLECHA_COMPILER}" --emit-object "${FLECHA_SOURCE}"
        --lib "std=${FLECHA_STD}" -o "${FLECHA_OUTPUT}.o"
    RESULT_VARIABLE compileResult
    ERROR_VARIABLE compileError
    TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR "UDP runtime fixture compilation failed: ${compileError}")
endif()

set(linkFlags)
if(FLECHA_SANITIZERS)
    list(APPEND linkFlags -fsanitize=address,undefined)
endif()
if(WIN32)
    list(APPEND linkFlags -lbcrypt -lws2_32)
endif()

execute_process(
    COMMAND "${FLECHA_LINKER}" "${FLECHA_HARNESS}" "${FLECHA_OUTPUT}.o"
        "-I${FLECHA_RUNTIME_INCLUDE}" "${FLECHA_RUNTIME}" ${linkFlags}
        -Wl,--wrap=flecha_runtime_v1_udp_socket_bind
        -Wl,--wrap=flecha_runtime_v1_udp_socket_send_to
        -Wl,--wrap=flecha_runtime_v1_udp_socket_receive_from
        -Wl,--wrap=flecha_runtime_v1_udp_socket_set_timeout
        -Wl,--wrap=flecha_runtime_v1_udp_socket_local_address
        -Wl,--wrap=flecha_runtime_v1_udp_socket_close
        -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE linkResult
    ERROR_VARIABLE linkError
    TIMEOUT 30
)
if(NOT linkResult EQUAL 0)
    message(FATAL_ERROR "UDP runtime fixture linking failed: ${linkError}")
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    ERROR_VARIABLE runError
    TIMEOUT 10
)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR
        "UDP runtime fixture returned ${runResult}, expected 42: ${runError}"
    )
endif()
