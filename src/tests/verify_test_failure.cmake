execute_process(
    COMMAND "${FLECHA_COMPILER}" test "${FLECHA_SOURCE}"
            --root "${FLECHA_ROOT}" --lib "std=${FLECHA_STD}" --verbose
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

if(status EQUAL 0)
    message(FATAL_ERROR "expected the fixture test to fail\n${output}${error}")
endif()

set(diagnostics "${output}${error}")
foreach(expected
        "reports_failure [\.]+ fail"
        "test: .*failure_test.fl:8:6"
        "assertion: actual is true"
        "message: expected fixture failure")
    if(NOT diagnostics MATCHES "${expected}")
        message(FATAL_ERROR
            "missing test diagnostic '${expected}'\n${diagnostics}")
    endif()
endforeach()
