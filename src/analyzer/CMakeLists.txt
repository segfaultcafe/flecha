add_library(analyzer STATIC
    overlay_source_provider.cpp
    source_overlays.cpp
    transport.cpp
)

target_include_directories(analyzer
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src/include
)

target_link_libraries(analyzer
    PUBLIC
        core
    PRIVATE
        flecha_project_options
)
