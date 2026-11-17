include(${PROJECT_SOURCE_DIR}/cmake/toolchain_policy.cmake)
find_package(LLVM CONFIG QUIET)
if(NOT LLVM_FOUND)
    message(FATAL_ERROR
        "LLVM development package was not found. Flecha requires LLVM "
        "${FLECHA_LLVM_MIN_MAJOR} through ${FLECHA_LLVM_MAX_MAJOR}. Install "
        "the LLVM development files and configure with "
        "-DLLVM_DIR=/path/to/lib/cmake/llvm (use 'llvm-config --cmakedir' "
        "to locate it)."
    )
endif()
include(${PROJECT_SOURCE_DIR}/cmake/check_llvm_version.cmake)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

# The top-level install rules use these directories to make release archives
# independent of the LLVM installation present on the build runner.
set(FLECHA_LLVM_RUNTIME_DIRS
    "${LLVM_LIBRARY_DIRS};${LLVM_TOOLS_BINARY_DIR}"
    PARENT_SCOPE
)

add_library(llvm_codegen STATIC
    async_code_generator.cpp
    async_frame_layout.cpp
    async_value_generator.cpp
    llvm_code_generator.cpp
    native_compiler.cpp
)

target_include_directories(llvm_codegen
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src/include
)

target_include_directories(llvm_codegen SYSTEM PUBLIC
        ${LLVM_INCLUDE_DIRS}
)

target_link_libraries(llvm_codegen
    PUBLIC
        lowering
    PRIVATE
        flecha_project_options
        LLVM
)

add_dependencies(llvm_codegen runtime)
target_compile_definitions(llvm_codegen PRIVATE
    FLECHA_RUNTIME_LIBRARY_PATH="$<TARGET_FILE:runtime>"
    FLECHA_RUNTIME_LIBRARY_NAME="$<TARGET_FILE_NAME:runtime>"
    FLECHA_RUNTIME_INSTALL_DIRECTORY="${CMAKE_INSTALL_LIBDIR}/flecha"
    FLECHA_OBJECT_SUFFIX="${CMAKE_CXX_OUTPUT_EXTENSION}"
)

if(FLECHA_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_definitions(llvm_codegen PRIVATE FLECHA_SANITIZED_RUNTIME=1)
endif()
