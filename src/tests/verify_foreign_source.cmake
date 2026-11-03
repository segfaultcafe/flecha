foreach(variable IN ITEMS FLECHA_COMPILER FLECHA_SOURCE FLECHA_OUTPUT)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${FLECHA_SOURCE}" -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE buildResult
    ERROR_VARIABLE buildError
    TIMEOUT 30
)
if(NOT buildResult EQUAL 0)
    message(FATAL_ERROR "foreign-source build failed: ${buildError}")
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    TIMEOUT 10
)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR
        "foreign-source executable returned ${runResult}, expected 42")
endif()

set(object "${FLECHA_OUTPUT}.o")
execute_process(
    COMMAND "${FLECHA_COMPILER}" --emit-object "${FLECHA_SOURCE}" -o "${object}"
    RESULT_VARIABLE objectResult
    ERROR_VARIABLE objectError
    TIMEOUT 30
)
if(NOT objectResult EQUAL 0 OR NOT EXISTS "${object}")
    message(FATAL_ERROR "foreign-source object emission failed: ${objectError}")
endif()

set(invalid "${FLECHA_OUTPUT}_invalid.fl")
file(WRITE "${invalid}"
    "foreign c {\nint broken(void) { this is not C; }\n}\n"
    "extern i32 broken();\ni32 main() { return broken(); }\n")
execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${invalid}"
        -o "${FLECHA_OUTPUT}_invalid"
    RESULT_VARIABLE invalidResult
    ERROR_VARIABLE invalidError
    TIMEOUT 30
)
if(invalidResult EQUAL 0)
    message(FATAL_ERROR "invalid embedded C unexpectedly compiled")
endif()
if(NOT invalidError MATCHES "FLECHA-C0005")
    message(FATAL_ERROR
        "invalid embedded C did not report FLECHA-C0005: ${invalidError}")
endif()

file(REMOVE "${FLECHA_OUTPUT}" "${object}" "${invalid}"
    "${FLECHA_OUTPUT}_invalid")
