cmake_minimum_required(VERSION 3.20)

project(Flecha VERSION 0.2.2 LANGUAGES C CXX)

include(cmake/check_compiler_version.cmake)
include(CTest)
include(GNUInstallDirs)
include(cmake/flecha_version.cmake)

configure_file(
    src/include/flecha/version.hpp.in
    ${PROJECT_BINARY_DIR}/generated/flecha/version.hpp
    @ONLY
)

option(FLECHA_ENABLE_SANITIZERS
    "Build Flecha with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep language and compiler requirements in one target so every Flecha target
# is built consistently without modifying global compiler flags.
add_library(flecha_project_options INTERFACE)
target_compile_features(flecha_project_options INTERFACE cxx_std_23)

if(FLECHA_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(flecha_project_options INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(flecha_project_options INTERFACE
            -fsanitize=address,undefined
        )
    else()
        message(WARNING
            "FLECHA_ENABLE_SANITIZERS is only configured for Clang and GCC"
        )
    endif()
endif()

add_subdirectory(src/diagnostics)
add_subdirectory(src/core)
add_subdirectory(src/semantic)
add_subdirectory(src/lowering)
add_subdirectory(src/runtime)
add_subdirectory(src/codegen)
add_subdirectory(src/memory)
add_subdirectory(src/utils)
add_subdirectory(src/std)

add_executable(flecha src/main.cpp)
target_include_directories(flecha PRIVATE ${PROJECT_BINARY_DIR}/generated)
target_link_libraries(flecha
    PRIVATE
        flecha_project_options
        diagnostics
        core
        semantic
        lowering
        llvm_codegen
        memory
        runtime
        utils
        flecha_std
)

add_executable(longbow src/longbow/main.cpp)
target_include_directories(longbow PRIVATE ${PROJECT_BINARY_DIR}/generated)
target_link_libraries(longbow PRIVATE flecha_project_options llvm_codegen)
target_compile_definitions(longbow PRIVATE
    FLECHA_EXECUTABLE_SUFFIX="${CMAKE_EXECUTABLE_SUFFIX}"
    FLECHA_OBJECT_SUFFIX="${CMAKE_CXX_OUTPUT_EXTENSION}"
    FLECHA_INSTALL_DATADIR="${CMAKE_INSTALL_DATADIR}"
)
add_dependencies(longbow flecha)

if(UNIX AND NOT APPLE)
    # BundleUtilities places non-system shared libraries beside the installed
    # executables. Make the Linux loader search that relocatable directory for
    # direct and transitive dependencies.
    set_target_properties(flecha longbow PROPERTIES INSTALL_RPATH "$ORIGIN")
    target_link_options(flecha PRIVATE -Wl,--disable-new-dtags)
    target_link_options(longbow PRIVATE -Wl,--disable-new-dtags)
endif()

install(TARGETS flecha longbow
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Make release archives runnable without the LLVM installation used by the
# build runner. BundleUtilities copies LLVM and its non-system dependencies;
# on macOS it also rewrites absolute Homebrew install names, and on Windows it
# places the required DLLs beside the executables.
install(CODE "
    include(BundleUtilities)
    set(BU_CHMOD_BUNDLE_ITEMS TRUE)
    function(gp_resolved_file_type_override resolved_file type_var)
        get_filename_component(_dependency_name \"\${resolved_file}\" NAME)
        if(_dependency_name MATCHES \"^libLLVM([.-]|$)\")
            if(APPLE)
                set(\${type_var} embedded PARENT_SCOPE)
            else()
                set(\${type_var} local PARENT_SCOPE)
            endif()
        elseif(UNIX AND NOT APPLE AND NOT _dependency_name MATCHES
                \"^(ld-linux|libc[.]so|libm[.]so|libdl[.]so|libpthread[.]so|librt[.]so|libgcc_s[.]so|libstdc|libresolv[.]so|libutil[.]so)\")
            # Linux distributions commonly install LLVM's optional runtime
            # dependencies under /usr/lib. BundleUtilities normally labels
            # every library there as a system component, even when it is not
            # part of the stable base runtime (for example libedit or libffi).
            set(\${type_var} local PARENT_SCOPE)
        endif()
    endfunction()
    function(gp_item_default_embedded_path_override item path_var)
        set(\${path_var} \"@executable_path\" PARENT_SCOPE)
    endfunction()
    set(_flecha_install_bin
        \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
    set(_flecha_dependency_dirs \"${FLECHA_LLVM_RUNTIME_DIRS}\")
    fixup_bundle(
        \"\${_flecha_install_bin}/flecha${CMAKE_EXECUTABLE_SUFFIX}\"
        \"\"
        \"\${_flecha_dependency_dirs}\"
    )
")

install(TARGETS runtime
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/flecha
)
install(DIRECTORY std/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/flecha/std
    FILES_MATCHING PATTERN "*.fl"
)
install(DIRECTORY editors/neovim/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/flecha/editors/neovim
    PATTERN "tests" EXCLUDE
)
install(DIRECTORY editors/vscode/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/flecha/editors/vscode
    PATTERN ".vscodeignore" EXCLUDE
    PATTERN "node_modules" EXCLUDE
    PATTERN "package-lock.json" EXCLUDE
    PATTERN "*.vsix" EXCLUDE
)
install(FILES README.md CHANGELOG.md CONTRIBUTING.md CODE_STYLE.md SPEC.md LICENSE
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/reference
    FILES_MATCHING PATTERN "*.md"
)

if(BUILD_TESTING)
    include(FetchContent)
    set(INSTALL_GTEST OFF CACHE BOOL "Do not install Flecha's test dependency" FORCE)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/release-1.12.1.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(src/tests)
endif()

set(CPACK_PACKAGE_NAME "Flecha")
set(CPACK_PACKAGE_VENDOR "Flecha")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME
    "flecha-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}"
)
set(CPACK_GENERATOR "TGZ;ZIP")
include(CPack)
