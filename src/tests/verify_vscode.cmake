foreach(variable IN ITEMS EDITOR_ROOT PROJECT_VERSION SOURCE_ROOT)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

file(READ "${EDITOR_ROOT}/package.json" manifest)
string(JSON packageVersion GET "${manifest}" version)
if(NOT packageVersion STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR
        "VS Code extension version ${packageVersion} does not match ${PROJECT_VERSION}")
endif()

string(JSON languageId GET "${manifest}" contributes languages 0 id)
string(JSON grammarScope GET "${manifest}" contributes grammars 0 scopeName)
if(NOT languageId STREQUAL "flecha" OR NOT grammarScope STREQUAL "source.flecha")
    message(FATAL_ERROR "VS Code manifest does not register the Flecha language")
endif()

string(JSON packageCommand GET "${manifest}" scripts package)
string(JSON vsceVersion GET "${manifest}" devDependencies "@vscode/vsce")
if(NOT packageCommand STREQUAL "vsce package --no-dependencies" OR
   NOT vsceVersion STREQUAL "4.0.0")
    message(FATAL_ERROR
        "VS Code packaging must use the pinned @vscode/vsce 4.0.0"
    )
endif()

foreach(file IN ITEMS language-configuration.json syntaxes/flecha.tmLanguage.json)
    file(READ "${EDITOR_ROOT}/${file}" json)
    string(JSON jsonType TYPE "${json}")
    if(NOT jsonType STREQUAL "OBJECT")
        message(FATAL_ERROR "${file} must contain a JSON object")
    endif()
endforeach()

foreach(file IN ITEMS .vscodeignore LICENSE package-lock.json README.md)
    if(NOT EXISTS "${EDITOR_ROOT}/${file}")
        message(FATAL_ERROR "VS Code packaging input is missing ${file}")
    endif()
endforeach()

file(READ "${SOURCE_ROOT}/LICENSE" projectLicense)
file(READ "${EDITOR_ROOT}/LICENSE" extensionLicense)
string(STRIP "${projectLicense}" projectLicense)
string(STRIP "${extensionLicense}" extensionLicense)
if(NOT extensionLicense STREQUAL projectLicense)
    message(FATAL_ERROR
        "VS Code extension LICENSE does not match the project license"
    )
endif()

file(READ "${EDITOR_ROOT}/package-lock.json" lockfile)
string(JSON lockedVsceVersion GET "${lockfile}"
       packages "" devDependencies "@vscode/vsce")
if(NOT lockedVsceVersion STREQUAL vsceVersion)
    message(FATAL_ERROR
        "package-lock.json does not pin @vscode/vsce ${vsceVersion}"
    )
endif()

file(READ "${EDITOR_ROOT}/syntaxes/flecha.tmLanguage.json" grammar)
foreach(required IN ITEMS
        enum match test func once copy own lend quiver typeset isof
        signed_integer unsigned_integer integer floating_point numeric
        "storage.type.function.flecha"
        "storage.type.typeset.flecha"
        "entity.name.type.typeset.flecha"
        "support.type.typeset.builtin.flecha" "=>")
    string(FIND "${grammar}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "VS Code grammar does not cover ${required}")
    endif()
endforeach()

foreach(required IN ITEMS
        "keyword.other.test.flecha" "#FFD700"
        "storage.type.function.flecha" "#569CD6"
        "support.type.typeset.builtin.flecha" "#87CEFA")
    string(FIND "${manifest}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "VS Code manifest does not style ${required}")
    endif()
endforeach()

file(READ "${SOURCE_ROOT}/.github/workflows/ci.yml" ciWorkflow)
file(READ "${SOURCE_ROOT}/.github/workflows/release.yml" releaseWorkflow)
file(READ "${SOURCE_ROOT}/docs/editors.md" editorDocumentation)

function(require_text textVariable expected label)
    string(FIND "${${textVariable}}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${label} is missing '${expected}'")
    endif()
endfunction()

foreach(workflow IN ITEMS ciWorkflow releaseWorkflow)
    require_text(
        ${workflow}
        "npm ci --prefix editors/vscode --ignore-scripts"
        "VSIX workflow lockfile installation"
    )
    require_text(
        ${workflow}
        "cmake/verify_vsix.cmake"
        "VSIX workflow verification"
    )
endforeach()
require_text(
    releaseWorkflow
    "release-assets/*.vsix"
    "VSIX release publication"
)
require_text(
    editorDocumentation
    "code --install-extension flecha-language-VERSION.vsix"
    "VS Code installation documentation"
)
