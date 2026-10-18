foreach(variable IN ITEMS FLECHA_COMPILER FLECHA_SOURCE FLECHA_OUTPUT)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

execute_process(
    COMMAND "${FLECHA_COMPILER}" check "${FLECHA_SOURCE}/main.fl"
        --root "${FLECHA_SOURCE}"
    RESULT_VARIABLE checkResult
    ERROR_VARIABLE checkError
    TIMEOUT 30
)
if(NOT checkResult EQUAL 0)
    message(FATAL_ERROR "callable integration check failed: ${checkError}")
endif()

execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${FLECHA_SOURCE}/main.fl"
        --root "${FLECHA_SOURCE}" -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE buildResult
    ERROR_VARIABLE buildError
    TIMEOUT 30
)
if(NOT buildResult EQUAL 0)
    message(FATAL_ERROR "callable integration build failed: ${buildError}")
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    OUTPUT_VARIABLE runOutput
    ERROR_VARIABLE runError
    TIMEOUT 10
)
if(NOT runResult EQUAL 42 OR NOT runOutput STREQUAL "")
    message(FATAL_ERROR
        "callable integration returned ${runResult}, expected 42: ${runOutput}${runError}")
endif()
