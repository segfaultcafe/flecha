add_library(analyzer STATIC
    analysis_session.cpp
    json.cpp
    overlay_source_provider.cpp
    protocol.cpp
    server_loop.cpp
    semantic_queries.cpp
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
        semantic
    PRIVATE
        flecha_project_options
)
