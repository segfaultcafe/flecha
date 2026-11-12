add_library(lowering STATIC
    bound_ir_dumper.cpp
    interface_layout.cpp
    lowerer.cpp
)

target_include_directories(lowering
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src/include
)

target_link_libraries(lowering
    PUBLIC
        semantic
    PRIVATE
        flecha_project_options
)
