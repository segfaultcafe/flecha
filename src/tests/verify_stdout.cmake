execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${FLECHA_SOURCE}" -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE compileResult
    ERROR_VARIABLE compileError
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR "native compilation failed: ${compileError}")
endif()

execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult
    OUTPUT_VARIABLE actualOutput
    ERROR_VARIABLE actualError
)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR "native program returned ${runResult}, expected 42")
endif()

set(expectedOutput [=[Flecha
8
16
32
64
72
8
16
32
64
72
1.25
1.5
true
false
!
]=])
if(NOT actualOutput STREQUAL expectedOutput)
    message(FATAL_ERROR
        "native output mismatch\nexpected:\n${expectedOutput}\nactual:\n${actualOutput}"
    )
endif()

if(NOT actualError STREQUAL expectedOutput)
    message(FATAL_ERROR
        "native standard error mismatch\nexpected:\n${expectedOutput}\nactual:\n${actualError}"
    )
endif()
