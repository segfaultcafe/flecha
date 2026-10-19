add_library(runtime STATIC runtime.cpp allocation.cpp filesystem.cpp
                           file_stream.cpp)
target_include_directories(runtime PUBLIC ${PROJECT_SOURCE_DIR}/src/include)
target_link_libraries(runtime PRIVATE flecha_project_options)

set_target_properties(runtime PROPERTIES
    OUTPUT_NAME flecha_runtime
    POSITION_INDEPENDENT_CODE ON
)
