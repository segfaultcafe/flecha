add_library(lsp STATIC
    transport.cpp
)

target_include_directories(lsp
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src/include
)

target_link_libraries(lsp
    PRIVATE
        flecha_project_options
)
