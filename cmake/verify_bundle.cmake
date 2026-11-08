if(NOT DEFINED INSTALL_ROOT)
    message(FATAL_ERROR "INSTALL_ROOT is required")
endif()

if(NOT DEFINED EXECUTABLE_SUFFIX)
    set(EXECUTABLE_SUFFIX "")
endif()

set(bundleBin "${INSTALL_ROOT}/bin")
set(compiler "${bundleBin}/flecha${EXECUTABLE_SUFFIX}")
set(longbow "${bundleBin}/longbow${EXECUTABLE_SUFFIX}")

foreach(executable IN ITEMS "${compiler}" "${longbow}")
    if(NOT EXISTS "${executable}")
        message(FATAL_ERROR "Missing installed executable: ${executable}")
    endif()
endforeach()

foreach(editorFile IN ITEMS
        share/flecha/editors/neovim/ftdetect/flecha.vim
        share/flecha/editors/neovim/plugin/longbow.vim
        share/flecha/editors/neovim/indent/flecha.vim
        share/flecha/editors/neovim/syntax/flecha.vim
        share/flecha/editors/vscode/language-configuration.json
        share/flecha/editors/vscode/package.json
        share/flecha/editors/vscode/dist/extension.js
        share/flecha/editors/vscode/syntaxes/flecha.tmLanguage.json)
    if(NOT EXISTS "${INSTALL_ROOT}/${editorFile}")
        message(FATAL_ERROR
            "Installed editor support is missing ${editorFile}"
        )
    endif()
endforeach()

if(EXISTS "${INSTALL_ROOT}/share/flecha/editors/vscode/node_modules")
    message(FATAL_ERROR
        "Installed VS Code support contains development dependencies"
    )
endif()

set(runtimeDependencyFilters)
if(WIN32)
    # Windows system DLLs can have optional or edition-specific dependencies
    # that are intentionally absent. Stop dependency traversal at the Windows
    # system directories so the verifier only audits redistributable DLLs.
    list(APPEND runtimeDependencyFilters
        PRE_EXCLUDE_REGEXES
        "^api-ms-win-.*"
        "^ext-ms-win-.*"
        POST_EXCLUDE_REGEXES
        "[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]([Ss][Yy][Ss][Tt][Ee][Mm]32|[Ss][Yy][Ss][Ww][Oo][Ww]64)[/\\\\]"
    )
endif()

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${compiler}" "${longbow}"
    RESOLVED_DEPENDENCIES_VAR resolvedDependencies
    UNRESOLVED_DEPENDENCIES_VAR unresolvedDependencies
    CONFLICTING_DEPENDENCIES_PREFIX dependencyConflicts
    ${runtimeDependencyFilters}
)

if(unresolvedDependencies)
    list(JOIN unresolvedDependencies ", " unresolvedText)
    message(FATAL_ERROR "Unresolved bundled dependencies: ${unresolvedText}")
endif()

if(dependencyConflicts_FILENAMES)
    list(JOIN dependencyConflicts_FILENAMES ", " conflictText)
    message(FATAL_ERROR "Conflicting bundled dependencies: ${conflictText}")
endif()

file(REAL_PATH "${bundleBin}" realBundleBin)
file(TO_CMAKE_PATH "${realBundleBin}" realBundleBin)
string(TOLOWER "${realBundleBin}/" bundlePrefix)
set(foundLlvm FALSE)

foreach(dependency IN LISTS resolvedDependencies)
    get_filename_component(dependencyName "${dependency}" NAME)
    if(dependencyName MATCHES "^(lib)?LLVM([.-]|$)")
        set(foundLlvm TRUE)
    endif()

    set(isSystemDependency FALSE)
    if(WIN32)
        file(TO_CMAKE_PATH "${dependency}" normalizedDependency)
        string(TOLOWER "${normalizedDependency}" normalizedDependency)
        if(normalizedDependency MATCHES "/windows/(system32|syswow64)/")
            set(isSystemDependency TRUE)
        endif()
    elseif(APPLE)
        if(dependency MATCHES "^(/System/Library/|/usr/lib/)")
            set(isSystemDependency TRUE)
        endif()
    else()
        if(dependencyName MATCHES
                "^(ld-linux|libc[.]so|libm[.]so|libdl[.]so|libpthread[.]so|librt[.]so|libgcc_s[.]so|libstdc|libresolv[.]so|libutil[.]so)")
            set(isSystemDependency TRUE)
        endif()
    endif()

    if(NOT isSystemDependency)
        file(REAL_PATH "${dependency}" realDependency)
        file(TO_CMAKE_PATH "${realDependency}" realDependency)
        string(TOLOWER "${realDependency}" comparableDependency)
        string(FIND "${comparableDependency}" "${bundlePrefix}" bundleIndex)
        if(NOT bundleIndex EQUAL 0)
            message(FATAL_ERROR
                "Dependency is outside the install bundle: ${dependency}")
        endif()
    endif()
endforeach()

if(NOT foundLlvm)
    message(FATAL_ERROR "The installed executables did not resolve bundled LLVM")
endif()

message(STATUS "Verified that non-system runtime dependencies are bundled")
