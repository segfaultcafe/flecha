add_library(lsp STATIC
    analyzer_client.cpp
    protocol.cpp
    server_loop.cpp
    uri.cpp
)

target_include_directories(lsp
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src/include
)

target_link_libraries(lsp
    PUBLIC
        analyzer
    PRIVATE
        flecha_project_options
        runtime
)

add_executable(flecha-lsp main.cpp)
target_link_libraries(flecha-lsp
    PRIVATE
        flecha_project_options
        lsp
)
target_compile_definitions(flecha-lsp PRIVATE
    FLECHA_EXECUTABLE_SUFFIX="${CMAKE_EXECUTABLE_SUFFIX}"
)
