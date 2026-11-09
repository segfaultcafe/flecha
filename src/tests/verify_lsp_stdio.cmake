foreach(required FLECHA_COMPILER FLECHA_LSP TEST_ROOT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(TO_CMAKE_PATH "${TEST_ROOT}" workspace)
set(documentPath "${workspace}/main.fl")
if(WIN32)
    set(workspaceUri "file:///${workspace}")
    set(documentUri "file:///${documentPath}")
else()
    set(workspaceUri "file://${workspace}")
    set(documentUri "file://${documentPath}")
endif()

file(WRITE "${documentPath}" "module main;\nvoid main() {}\n")
set(initialize
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":\"${workspaceUri}\"}}")
set(initialized
    "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}")
set(openDocument
    "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"${documentUri}\",\"languageId\":\"flecha\",\"version\":1,\"text\":\"module main;\\nvoid main() { ` }\"}}}")
set(shutdown
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\",\"params\":{}}")
set(exit "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}")

set(input "${TEST_ROOT}/lsp-input.txt")
file(WRITE "${input}" "")
foreach(message initialize initialized openDocument shutdown exit)
    string(LENGTH "${${message}}" messageLength)
    file(APPEND "${input}"
        "Content-Length: ${messageLength}\r\n\r\n${${message}}")
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "FLECHA_COMPILER=${FLECHA_COMPILER}"
        "${FLECHA_LSP}"
    INPUT_FILE "${input}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 30
)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "flecha-lsp exited with ${result}:\n${error}\n${output}")
endif()
if(NOT error STREQUAL "")
    message(FATAL_ERROR "flecha-lsp wrote to stderr:\n${error}")
endif()
foreach(expected
        "\"id\":1,\"result\""
        "\"textDocumentSync\""
        "textDocument/publishDiagnostics"
        "FLECHA-L0001"
        "\"version\":1"
        "\"id\":2,\"result\":null")
    string(FIND "${output}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "LSP output does not contain ${expected}:\n${output}")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
