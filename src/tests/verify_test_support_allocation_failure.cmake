execute_process(
    COMMAND "${FLECHA_COMPILER}" --emit-object "${FLECHA_SOURCE}"
        --lib "std=${FLECHA_STD}" -o "${FLECHA_OUTPUT}.o"
    RESULT_VARIABLE compileResult
    ERROR_VARIABLE compileError
    TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR
        "test support allocation fixture compilation failed: ${compileError}"
    )
endif()

set(sanitizerFlags)
if(FLECHA_SANITIZERS)
    list(APPEND sanitizerFlags -fsanitize=address,undefined)
endif()
execute_process(
    COMMAND "${FLECHA_LINKER}" "${FLECHA_HARNESS}" "${FLECHA_OUTPUT}.o"
        "${FLECHA_RUNTIME}" ${sanitizerFlags}
        -Wl,--wrap=flecha_runtime_v1_alloc_array
        -Wl,--wrap=malloc
        -Wl,--wrap=realloc
        -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE linkResult
    ERROR_VARIABLE linkError
    TIMEOUT 30
)
if(NOT linkResult EQUAL 0)
    message(FATAL_ERROR
        "test support allocation fixture linking failed: ${linkError}"
    )
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    OUTPUT_VARIABLE output
    ERROR_VARIABLE runError
    TIMEOUT 10
)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR
        "test support allocation fixture returned ${runResult}, expected 42: ${runError}"
    )
endif()

foreach(expected
        "assertion: golden text file is readable\n    message: golden read allocation fallback"
        "assertion: actual == expected\n    message: golden text diagnostic fallback"
        "assertion: actual bytes == golden bytes\n    message: golden byte diagnostic fallback")
    string(FIND "${output}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "missing allocation fallback diagnostic '${expected}'\n${output}"
        )
    endif()
endforeach()
