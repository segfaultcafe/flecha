add_library(longbow_model STATIC
    manifest.cpp
    resolver.cpp
)

target_include_directories(longbow_model
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src/include
)

target_link_libraries(longbow_model
    PUBLIC
        core
    PRIVATE
        flecha_project_options
)
