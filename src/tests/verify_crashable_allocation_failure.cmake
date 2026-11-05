foreach(variable IN ITEMS
        FLECHA_COMPILER FLECHA_SOURCE FLECHA_STD FLECHA_HARNESS
        FLECHA_OUTPUT FLECHA_RUNTIME FLECHA_LINKER
        FLECHA_STRING_FAIL_CALL FLECHA_EXPECTED_MESSAGE)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

if(NOT DEFINED FLECHA_ALLOCATION_SYMBOL OR
   "${FLECHA_ALLOCATION_SYMBOL}" STREQUAL "")
    set(FLECHA_ALLOCATION_SYMBOL realloc)
endif()

if(FLECHA_ALLOCATION_SYMBOL STREQUAL "array")
    set(failureDefinition
        "-DFLECHA_ARRAY_FAIL_CALL=${FLECHA_STRING_FAIL_CALL}")
    set(wrapFlag
        "-Wl,--wrap=flecha_runtime_v1_alloc_array,--wrap=flecha_runtime_v1_alloc,--wrap=flecha_runtime_v1_free")
else()
    set(failureDefinition
        "-DFLECHA_STRING_FAIL_CALL=${FLECHA_STRING_FAIL_CALL}")
    set(wrapFlag "-Wl,--wrap=${FLECHA_ALLOCATION_SYMBOL}")
endif()

execute_process(
    COMMAND "${FLECHA_COMPILER}" --emit-object "${FLECHA_SOURCE}"
        --lib "std=${FLECHA_STD}" -o "${FLECHA_OUTPUT}.o"
    RESULT_VARIABLE compileResult
    ERROR_VARIABLE compileError
    TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR
        "crashable allocation fixture compilation failed: ${compileError}"
    )
endif()

set(sanitizerFlags)
if(FLECHA_SANITIZERS)
    list(APPEND sanitizerFlags -fsanitize=address,undefined)
endif()

execute_process(
    COMMAND "${FLECHA_LINKER}" "${FLECHA_HARNESS}" "${FLECHA_OUTPUT}.o"
        "${failureDefinition}"
        "${FLECHA_RUNTIME}" ${sanitizerFlags}
        "${wrapFlag}"
        -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE linkResult
    ERROR_VARIABLE linkError
    TIMEOUT 30
)
if(NOT linkResult EQUAL 0)
    message(FATAL_ERROR
        "crashable allocation fixture linking failed: ${linkError}"
    )
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    ERROR_VARIABLE runError
    TIMEOUT 10
)
if(NOT runResult MATCHES "Subprocess aborted|^134$")
    message(FATAL_ERROR
        "crashable allocation fixture terminated with '${runResult}', expected SIGABRT\nstderr:\n${runError}"
    )
endif()

foreach(expected IN ITEMS
        "flecha: unrecoverable crash"
        "message: ${FLECHA_EXPECTED_MESSAGE}"
        "registers:"
        "stack trace:")
    string(FIND "${runError}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "crashable allocation report is missing '${expected}'\nstderr:\n${runError}"
        )
    endif()
endforeach()
