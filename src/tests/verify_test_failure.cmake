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
        "message: expected fixture failure"
        "reports_less_failure [\.]+ fail"
        "assertion: actual < expected [(]actual=5, expected=4[)]"
        "message: expected less failure"
        "reports_less_or_equal_failure [\.]+ fail"
        "assertion: actual <= expected [(]actual=5, expected=4[)]"
        "message: expected less-or-equal failure"
        "reports_greater_failure [\.]+ fail"
        "assertion: actual > expected [(]actual=4, expected=5[)]"
        "message: expected greater failure"
        "reports_greater_or_equal_failure [\.]+ fail"
        "assertion: actual >= expected [(]actual=4, expected=5[)]"
        "message: expected greater-or-equal failure"
        "reports_numeric_equal_failure [\.]+ fail"
        "assertion: actual == expected [(]actual=4, expected=5[)]"
        "message: expected numeric equality failure"
        "reports_numeric_not_equal_failure [\.]+ fail"
        "assertion: actual != unexpected [(]actual=5, unexpected=5[)]"
        "message: expected numeric inequality failure"
        "reports_string_equal_failure [\.]+ fail"
        "assertion: actual == expected [(]actual=\"hello\", expected=\"world\"[)]"
        "message: expected string equality failure"
        "reports_string_not_equal_failure [\.]+ fail"
        "assertion: actual != unexpected [(]actual=\"same\", unexpected=\"same\"[)]"
        "message: expected string inequality failure"
        "reports_escaped_string_equal_failure [\.]+ fail"
        "message: expected escaped string equality failure"
        "reports_escaped_string_not_equal_failure [\.]+ fail"
        "message: expected escaped string inequality failure"
        "reports_string_preview_boundary_failure [\.]+ fail"
        "message: expected string preview boundary failure"
        "reports_string_single_omission_failure [\.]+ fail"
        "message: expected single-byte omission failure"
        "reports_large_string_truncation_failure [\.]+ fail"
        "message: expected large string truncation failure"
        "reports_bytes_equal_length_failure [\.]+ fail"
        "assertion: actual bytes == expected bytes [(]actual length=2, expected length=3[)]"
        "message: expected byte length failure"
        "reports_bytes_equal_content_failure [\.]+ fail"
        "assertion: actual bytes == expected bytes [(]index=1, actual=2, expected=9[)]"
        "message: expected byte content failure"
        "reports_bytes_not_equal_identical_failure [\.]+ fail"
        "assertion: actual bytes != unexpected bytes [(]both length=3[)]"
        "message: expected identical byte failure"
        "reports_bytes_not_equal_empty_failure [\.]+ fail"
        "assertion: actual bytes != unexpected bytes [(]both length=0[)]"
        "message: expected empty byte failure"
        "reports_different_error_variant_failure [\.]+ fail"
        "assertion: actual has expected error variant"
        "message: expected same-family variant failure"
        "reports_different_error_family_failure [\.]+ fail"
        "message: expected cross-family variant failure"
        "reports_missing_error_variant_failure [\.]+ fail"
        "message: expected missing-error variant failure"
        "reports_missing_error_family_failure [\.]+ fail"
        "assertion: actual has expected error family"
        "message: expected missing error family failure"
        "reports_generic_error_family_failure [\.]+ fail"
        "message: expected generic error family failure")
    if(NOT diagnostics MATCHES "${expected}")
        message(FATAL_ERROR
            "missing test diagnostic '${expected}'\n${diagnostics}")
    endif()
endforeach()

set(escaped_equality_diagnostic
    [=[assertion: actual == expected (actual="\"\\\n\r\t\0\x01", expected="plain")]=])
string(FIND "${diagnostics}" "${escaped_equality_diagnostic}"
    escaped_equality_position)
if(escaped_equality_position EQUAL -1)
    message(FATAL_ERROR
        "missing escaped string diagnostic '${escaped_equality_diagnostic}'\n${diagnostics}")
endif()

set(escaped_inequality_diagnostic
    [=[assertion: actual != unexpected (actual="\"\\\n\r\t\0\x01", unexpected="\"\\\n\r\t\0\x01")]=])
string(FIND "${diagnostics}" "${escaped_inequality_diagnostic}"
    escaped_inequality_position)
if(escaped_inequality_position EQUAL -1)
    message(FATAL_ERROR
        "missing escaped string diagnostic '${escaped_inequality_diagnostic}'\n${diagnostics}")
endif()

string(REPEAT "A" 64 preview_boundary)
set(preview_boundary_diagnostic
    "assertion: actual == expected (actual=\"${preview_boundary}\", expected=\"different\")")
string(FIND "${diagnostics}" "${preview_boundary_diagnostic}"
    preview_boundary_position)
if(preview_boundary_position EQUAL -1)
    message(FATAL_ERROR
        "missing string boundary diagnostic '${preview_boundary_diagnostic}'\n${diagnostics}")
endif()

string(REPEAT "A" 32 preview_edge)
set(single_omission_diagnostic
    "assertion: actual == expected (actual=\"${preview_edge}...<1 byte omitted>...${preview_edge}\", expected=\"different\")")
string(FIND "${diagnostics}" "${single_omission_diagnostic}"
    single_omission_position)
if(single_omission_position EQUAL -1)
    message(FATAL_ERROR
        "missing single-byte omission diagnostic '${single_omission_diagnostic}'\n${diagnostics}")
endif()

string(REPEAT "A" 31 preview_suffix)
set(large_truncation_diagnostic
    "assertion: actual == expected (actual=\"${preview_edge}...<16 bytes omitted>...${preview_suffix}Z\", expected=\"different\")")
string(FIND "${diagnostics}" "${large_truncation_diagnostic}"
    large_truncation_position)
if(large_truncation_position EQUAL -1)
    message(FATAL_ERROR
        "missing large string diagnostic '${large_truncation_diagnostic}'\n${diagnostics}")
endif()
