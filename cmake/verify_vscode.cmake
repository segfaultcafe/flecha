if(NOT DEFINED INSTALL_ROOT OR "${INSTALL_ROOT}" STREQUAL "")
    message(FATAL_ERROR "INSTALL_ROOT is required")
endif()

if(NOT DEFINED EXECUTABLE_SUFFIX)
    set(EXECUTABLE_SUFFIX "")
endif()

set(vscodeRoot "${INSTALL_ROOT}/share/flecha/editors/vscode")
set(vscodeServer "${INSTALL_ROOT}/bin/flecha-lsp${EXECUTABLE_SUFFIX}")
set(vscodeCompiler "${INSTALL_ROOT}/bin/flecha${EXECUTABLE_SUFFIX}")

foreach(required IN ITEMS
        "${vscodeServer}"
        "${vscodeCompiler}"
        "${vscodeRoot}/package.json"
        "${vscodeRoot}/dist/extension.js")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR
            "Installed VS Code language support is missing ${required}")
    endif()
endforeach()

# The extension lives at share/flecha/editors/vscode. Confirm that its
# four-level relative lookup reaches this installation instead of the build
# tree or the original archive location.
file(REAL_PATH "${vscodeRoot}/../../../.." vscodeInstallRoot)
file(REAL_PATH "${INSTALL_ROOT}" expectedInstallRoot)
if(NOT vscodeInstallRoot STREQUAL expectedInstallRoot)
    message(FATAL_ERROR
        "VS Code support does not resolve to the relocated install root: "
        "${vscodeInstallRoot}")
endif()

file(REAL_PATH
    "${vscodeRoot}/../../../../bin/flecha-lsp${EXECUTABLE_SUFFIX}"
    relativeServer)
file(REAL_PATH "${vscodeServer}" expectedServer)
if(NOT relativeServer STREQUAL expectedServer)
    message(FATAL_ERROR
        "VS Code support resolves the wrong language server: ${relativeServer}")
endif()

file(REAL_PATH
    "${vscodeRoot}/../../../../bin/flecha${EXECUTABLE_SUFFIX}"
    relativeCompiler)
file(REAL_PATH "${vscodeCompiler}" expectedCompiler)
if(NOT relativeCompiler STREQUAL expectedCompiler)
    message(FATAL_ERROR
        "VS Code support resolves the wrong compiler: ${relativeCompiler}")
endif()

file(READ "${vscodeRoot}/package.json" vscodeManifest)
foreach(setting IN ITEMS flecha.server.path flecha.compiler.path)
    string(JSON configuredDefault GET "${vscodeManifest}"
        contributes configuration properties "${setting}" default)
    if(NOT configuredDefault STREQUAL "")
        message(FATAL_ERROR
            "${setting} must default to automatic bundle/PATH discovery")
    endif()
endforeach()

file(READ "${vscodeRoot}/dist/extension.js" vscodeClient)
string(FIND "${vscodeClient}"
    "FLECHA_BUNDLED_TOOL_LAYOUT" bundledLayoutMarker)
if(bundledLayoutMarker EQUAL -1)
    message(FATAL_ERROR
        "Installed VS Code client does not contain relocatable tool discovery")
endif()

message(STATUS
    "Verified relocatable VS Code tools ${relativeServer} and ${relativeCompiler}")
