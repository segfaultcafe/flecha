foreach(required FLECHA_COMPILER PROJECT_ROOT PROJECT_VERSION ANALYZER_INPUT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(TO_CMAKE_PATH "${PROJECT_ROOT}" workspace)
set(documentPath "${workspace}/analyzer_stdio_overlay.fl")
set(initialize
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolMajor\":1,\"protocolMinor\":0,\"workspace\":\"${workspace}\"}}")
set(openDocument
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"document/open\",\"params\":{\"path\":\"${documentPath}\",\"version\":1,\"text\":\"module analyzer_stdio_overlay;\\nvoid main() { ` }\"}}")
set(invalidDiagnostics
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"document/diagnostics\",\"params\":{\"path\":\"${documentPath}\"}}")
set(changeDocument
    "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"document/change\",\"params\":{\"path\":\"${documentPath}\",\"version\":2,\"text\":\"module analyzer_stdio_overlay;\\nvoid main() {}\"}}")
set(validDiagnostics
    "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"document/diagnostics\",\"params\":{\"path\":\"${documentPath}\"}}")
set(closeDocument
    "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"document/close\",\"params\":{\"path\":\"${documentPath}\"}}")
set(shutdown
    "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"shutdown\",\"params\":{}}")

file(WRITE "${ANALYZER_INPUT}" "")
file(WRITE "${documentPath}"
    "module analyzer_stdio_overlay;\nvoid main() {}\n")
foreach(message
        initialize
        openDocument
        invalidDiagnostics
        changeDocument
        validDiagnostics
        closeDocument
        shutdown)
    string(LENGTH "${${message}}" messageLength)
    file(APPEND "${ANALYZER_INPUT}"
        "Content-Length: ${messageLength}\r\n\r\n${${message}}")
endforeach()

execute_process(
    COMMAND "${FLECHA_COMPILER}" analyze --stdio
    INPUT_FILE "${ANALYZER_INPUT}"
    RESULT_VARIABLE analyzerResult
    OUTPUT_VARIABLE analyzerOutput
    ERROR_VARIABLE analyzerError
)
file(REMOVE "${ANALYZER_INPUT}" "${documentPath}")

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
        "\"id\":2,\"result\":{\"status\":\"applied\"}"
        "\"id\":3,\"result\":{\"path\":\"${documentPath}\",\"version\":1,\"diagnostics\":[{"
        "\"id\":4,\"result\":{\"status\":\"applied\"}"
        "\"id\":5,\"result\":{\"path\":\"${documentPath}\",\"version\":2,\"diagnostics\":[]}"
        "\"id\":6,\"result\":{\"closed\":true}"
        "\"id\":7"
        "\"result\":null")
    string(FIND "${analyzerOutput}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "analyzer response does not contain ${expected}:\n${analyzerOutput}")
    endif()
endforeach()
