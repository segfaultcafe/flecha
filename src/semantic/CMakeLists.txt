add_library(semantic STATIC
    allocation_checker.cpp
    borrow_checker.cpp
    control_flow_graph.cpp
    definite_assignment.cpp
    generic_expander.cpp
    method_contract.cpp
    method_key.cpp
    scope.cpp
    semantic_analyzer.cpp
    semantic_model.cpp
    semantic_type.cpp
)

target_include_directories(semantic
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src/include
)

target_link_libraries(semantic
    PUBLIC
        core
        diagnostics
    PRIVATE
        flecha_project_options
)
