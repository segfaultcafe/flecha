execute_process(
    COMMAND "${FLECHA_COMPILER}" --emit-object "${FLECHA_SOURCE}"
        --lib "std=${FLECHA_STD}" -o "${FLECHA_OUTPUT}.o"
    RESULT_VARIABLE compileResult
    ERROR_VARIABLE compileError
    TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR
        "TCP runtime fixture compilation failed: ${compileError}"
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
    COMMAND "${FLECHA_LINKER}" "${FLECHA_HARNESS}" "${FLECHA_OUTPUT}.o"
        "-I${FLECHA_RUNTIME_INCLUDE}" "${FLECHA_RUNTIME}" ${linkFlags}
        -Wl,--wrap=flecha_runtime_v1_tcp_stream_connect
        -Wl,--wrap=flecha_runtime_v1_tcp_listener_bind
        -Wl,--wrap=flecha_runtime_v1_tcp_listener_accept
        -Wl,--wrap=flecha_runtime_v1_tcp_stream_read
        -Wl,--wrap=flecha_runtime_v1_tcp_stream_write
        -Wl,--wrap=flecha_runtime_v1_tcp_stream_set_timeout
        -Wl,--wrap=flecha_runtime_v1_tcp_stream_shutdown
        -Wl,--wrap=flecha_runtime_v1_tcp_stream_local_address
        -Wl,--wrap=flecha_runtime_v1_tcp_stream_peer_address
        -Wl,--wrap=flecha_runtime_v1_tcp_listener_local_address
        -Wl,--wrap=flecha_runtime_v1_tcp_stream_close
        -Wl,--wrap=flecha_runtime_v1_tcp_listener_close
        -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE linkResult
    ERROR_VARIABLE linkError
    TIMEOUT 30
)
if(NOT linkResult EQUAL 0)
    message(FATAL_ERROR "TCP runtime fixture linking failed: ${linkError}")
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    ERROR_VARIABLE runError
    TIMEOUT 10
)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR
        "TCP runtime fixture returned ${runResult}, expected 42: ${runError}"
    )
endif()
