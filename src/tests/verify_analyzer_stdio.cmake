foreach(required FLECHA_COMPILER PROJECT_ROOT PROJECT_VERSION ANALYZER_INPUT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(TO_CMAKE_PATH "${PROJECT_ROOT}" workspace)
set(initialize
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolMajor\":1,\"protocolMinor\":0,\"workspace\":\"${workspace}\"}}")
set(shutdown
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\",\"params\":{}}")
string(LENGTH "${initialize}" initializeLength)
string(LENGTH "${shutdown}" shutdownLength)
file(WRITE "${ANALYZER_INPUT}"
    "Content-Length: ${initializeLength}\r\n\r\n${initialize}"
    "Content-Length: ${shutdownLength}\r\n\r\n${shutdown}")

execute_process(
    COMMAND "${FLECHA_COMPILER}" analyze --stdio
    INPUT_FILE "${ANALYZER_INPUT}"
    RESULT_VARIABLE analyzerResult
    OUTPUT_VARIABLE analyzerOutput
    ERROR_VARIABLE analyzerError
)
file(REMOVE "${ANALYZER_INPUT}")

if(NOT analyzerResult EQUAL 0)
    message(FATAL_ERROR
        "analyzer exited with ${analyzerResult}:\n${analyzerError}")
endif()
if(NOT analyzerError STREQUAL "")
    message(FATAL_ERROR "analyzer wrote to stderr:\n${analyzerError}")
endif()

foreach(expected
        "\"id\":1"
        "\"compilerVersion\":\"${PROJECT_VERSION}\""
        "\"id\":2"
        "\"result\":null")
    string(FIND "${analyzerOutput}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "analyzer response does not contain ${expected}:\n${analyzerOutput}")
    endif()
endforeach()
