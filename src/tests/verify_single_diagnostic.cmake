foreach(variable IN ITEMS FLECHA_COMPILER FLECHA_SOURCE EXPECTED_DIAGNOSTIC)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

execute_process(
    COMMAND "${FLECHA_COMPILER}" check "${FLECHA_SOURCE}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

if(status EQUAL 0)
    message(FATAL_ERROR "expected fixture '${FLECHA_SOURCE}' to fail")
endif()

set(diagnostics "${output}${error}")
string(FIND "${diagnostics}" "${EXPECTED_DIAGNOSTIC}" position)
if(position EQUAL -1)
    message(FATAL_ERROR
        "missing diagnostic '${EXPECTED_DIAGNOSTIC}'\n${diagnostics}")
endif()
