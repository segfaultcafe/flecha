foreach(variable IN ITEMS FLECHA_COMPILER FLECHA_SOURCE)
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
    message(FATAL_ERROR
        "expected the invalid interface hierarchy fixture to fail")
endif()

set(diagnostics "${output}${error}")
foreach(expected IN ITEMS
        "unknown interface 'Missing'"
        "'Concrete' is not an interface"
        "interface 'Recursive' cannot inherit from itself"
        "interface inheritance cycle involving 'First'"
        "interface 'Child' already extends 'Root' in interface composition"
        "interface composition has a diamond through 'Root'"
        "duplicate interface conformance 'Root'")
    string(FIND "${diagnostics}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "missing interface diagnostic '${expected}'\n${diagnostics}")
    endif()
endforeach()
