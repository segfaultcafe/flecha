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
        "expected the invalid interface method-contract fixture to fail")
endif()

set(diagnostics "${output}${error}")
foreach(expected IN ITEMS
        "structure 'MissingImplementation' does not implement 'bar' required by interface 'MissingRequirement'"
        "method 'convert' changes the inherited contract from 'ChangedResultRoot'"
        "default method 'reset' cannot become virtual again"
        "final method 'close' cannot be redeclared"
        "interface composition has conflicting method 'collide' in structure"
        "default interface method 'supplied' requires exactly one out-of-line body"
        "final interface method 'fixed' cannot be overridden"
        "method 'reveal' is protected")
    string(FIND "${diagnostics}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "missing interface method-contract diagnostic '${expected}'\n${diagnostics}")
    endif()
endforeach()
