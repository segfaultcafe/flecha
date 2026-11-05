execute_process(
    COMMAND "${FLECHA_COMPILER}" test "${FLECHA_SOURCE}"
        --root "${FLECHA_ROOT}" --lib "std=${FLECHA_STD}" --verbose
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

if(status EQUAL 0)
    message(FATAL_ERROR "expected the golden fixture test to fail\n${output}${error}")
endif()

set(diagnostics "${output}${error}")
foreach(expected
        "reports_text_golden_mismatch [\.]+ fail"
        "assertion: actual == expected [(]actual=\"actual\", expected=\"expected\"[)]"
        "message: expected text golden mismatch"
        "reports_byte_golden_content_mismatch [\.]+ fail"
        "assertion: actual bytes == golden bytes [(]index=1, actual=9, golden=2[)]"
        "message: expected byte golden content mismatch"
        "reports_byte_golden_length_mismatch [\.]+ fail"
        "assertion: actual bytes == golden bytes [(]actual length=2, golden length=3[)]"
        "message: expected byte golden length mismatch"
        "reports_missing_golden_file [\.]+ fail"
        "assertion: golden text file is readable"
        "message: expected missing golden failure")
    if(NOT diagnostics MATCHES "${expected}")
        message(FATAL_ERROR
            "missing golden diagnostic '${expected}'\n${diagnostics}"
        )
    endif()
endforeach()
