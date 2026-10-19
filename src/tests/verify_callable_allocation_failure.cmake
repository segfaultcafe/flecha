foreach(variable IN ITEMS
        FLECHA_COMPILER FLECHA_SOURCE FLECHA_HARNESS FLECHA_OUTPUT
        FLECHA_RUNTIME FLECHA_LINKER)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

execute_process(
    COMMAND "${FLECHA_COMPILER}" --emit-object "${FLECHA_SOURCE}"
        -o "${FLECHA_OUTPUT}.o"
    RESULT_VARIABLE compileResult
    ERROR_VARIABLE compileError
    TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR "native compilation failed: ${compileError}")
endif()

set(sanitizerFlags)
if(FLECHA_SANITIZERS)
    list(APPEND sanitizerFlags -fsanitize=address,undefined)
endif()
execute_process(
    COMMAND "${FLECHA_LINKER}" "${FLECHA_HARNESS}" "${FLECHA_OUTPUT}.o"
        "${FLECHA_RUNTIME}" ${sanitizerFlags}
        -Wl,--wrap=flecha_runtime_v1_alloc -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE linkResult
    ERROR_VARIABLE linkError
    TIMEOUT 30
)
if(NOT linkResult EQUAL 0)
    message(FATAL_ERROR "native link failed: ${linkError}")
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    OUTPUT_VARIABLE runOutput
    ERROR_VARIABLE runError
    TIMEOUT 10
)
if(runResult EQUAL 0)
    message(FATAL_ERROR "closure allocation failure unexpectedly returned")
endif()
string(FIND "${runError}" "flecha runtime error: allocation failed" messageAt)
if(messageAt EQUAL -1)
    message(FATAL_ERROR
        "missing closure allocation failure diagnostic: ${runOutput}${runError}")
endif()
