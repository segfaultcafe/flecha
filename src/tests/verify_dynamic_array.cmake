execute_process(
    COMMAND "${FLECHA_COMPILER}" --emit-object "${FLECHA_SOURCE}"
        --lib "std=${FLECHA_STD}" -o "${FLECHA_OUTPUT}.o"
    RESULT_VARIABLE compileResult ERROR_VARIABLE compileError TIMEOUT 30)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR "dynamic array compilation failed: ${compileError}")
endif()
if(NOT DEFINED FLECHA_ARRAY_FAIL_CALL)
    set(FLECHA_ARRAY_FAIL_CALL 2)
endif()
set(sanitizerFlags)
if(FLECHA_SANITIZERS)
    list(APPEND sanitizerFlags -fsanitize=address,undefined)
endif()
execute_process(
    COMMAND "${FLECHA_LINKER}" "${FLECHA_HARNESS}" "${FLECHA_OUTPUT}.o"
        "-DFLECHA_ARRAY_FAIL_CALL=${FLECHA_ARRAY_FAIL_CALL}"
        "${FLECHA_RUNTIME}" ${sanitizerFlags}
        -Wl,--wrap=flecha_runtime_v1_alloc_array,--wrap=flecha_runtime_v1_alloc,--wrap=flecha_runtime_v1_free
        -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE result ERROR_VARIABLE errors TIMEOUT 30)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "dynamic array link failed: ${errors}")
endif()
execute_process(COMMAND "${FLECHA_OUTPUT}" RESULT_VARIABLE result ERROR_VARIABLE errors TIMEOUT 10)
if(NOT result EQUAL 42)
    message(FATAL_ERROR "dynamic array returned ${result}, expected 42: ${errors}")
endif()
