set(rootArguments)
if(DEFINED FLECHA_ROOT)
    list(APPEND rootArguments --root "${FLECHA_ROOT}")
endif()
execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${FLECHA_SOURCE}" ${rootArguments} -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "behavior compilation failed: ${output}${error}")
endif()
execute_process(COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT status EQUAL 42 OR NOT output STREQUAL "" OR NOT error STREQUAL "")
    message(FATAL_ERROR "behavior returned ${status}, expected 42: ${output}${error}")
endif()
