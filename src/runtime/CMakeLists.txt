add_library(runtime STATIC runtime.cpp allocation.cpp environment.cpp
                           filesystem.cpp file_stream.cpp process.cpp time.cpp
                           standard_stream.cpp)
target_include_directories(runtime PUBLIC ${PROJECT_SOURCE_DIR}/src/include)
target_link_libraries(runtime PRIVATE flecha_project_options)
if(WIN32)
    target_link_libraries(runtime PRIVATE bcrypt)
endif()

set_target_properties(runtime PROPERTIES
    OUTPUT_NAME flecha_runtime
    POSITION_INDEPENDENT_CODE ON
)
