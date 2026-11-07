add_library(analyzer STATIC
    transport.cpp
)

target_include_directories(analyzer
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src/include
)

target_link_libraries(analyzer
    PRIVATE
        flecha_project_options
)
