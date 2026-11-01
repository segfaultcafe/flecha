foreach(variable IN ITEMS FLECHA_COMPILER FLECHA_SOURCE FLECHA_OUTPUT)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

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
if(NOT runResult MATCHES "Subprocess aborted|^134$")
    message(FATAL_ERROR
        "crash fixture terminated with '${runResult}', expected SIGABRT")
endif()

foreach(expected IN ITEMS
        "flecha: unrecoverable crash"
        "message: deliberate fixture crash"
        "location: ${FLECHA_SOURCE}:2:5"
        "leaf"
        "registers:"
        "stack trace:")
    string(FIND "${actualError}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "crash report is missing '${expected}'\nstderr:\n${actualError}")
    endif()
endforeach()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    string(FIND "${actualError}" "RIP=" registerPosition)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    string(FIND "${actualError}" "PC=" registerPosition)
else()
    set(registerPosition 0)
endif()
if(registerPosition EQUAL -1)
    message(FATAL_ERROR "crash report did not contain host registers")
endif()
