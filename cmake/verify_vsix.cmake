foreach(variable IN ITEMS VSIX EDITOR_ROOT OUTPUT_ROOT)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

file(READ "${EDITOR_ROOT}/package.json" sourceManifest)
string(JSON sourceName GET "${sourceManifest}" name)
string(JSON sourcePublisher GET "${sourceManifest}" publisher)
string(JSON sourceVersion GET "${sourceManifest}" version)

if(DEFINED EXPECTED_VERSION AND
   NOT "${EXPECTED_VERSION}" STREQUAL "${sourceVersion}")
    message(FATAL_ERROR
        "VS Code extension version ${sourceVersion} does not match "
        "${EXPECTED_VERSION}"
    )
endif()

get_filename_component(vsixName "${VSIX}" NAME)
set(expectedVsixName "${sourceName}-${sourceVersion}.vsix")
if(NOT vsixName STREQUAL expectedVsixName)
    message(FATAL_ERROR
        "Expected VSIX name ${expectedVsixName}, got ${vsixName}"
    )
endif()

file(REMOVE_RECURSE "${OUTPUT_ROOT}")
file(MAKE_DIRECTORY "${OUTPUT_ROOT}")
file(ARCHIVE_EXTRACT INPUT "${VSIX}" DESTINATION "${OUTPUT_ROOT}")

foreach(file IN ITEMS
        "[Content_Types].xml"
        extension.vsixmanifest
        extension/LICENSE.txt
        extension/dist/extension.js
        extension/readme.md
        extension/language-configuration.json
        extension/package.json
        extension/syntaxes/flecha.tmLanguage.json)
    if(NOT EXISTS "${OUTPUT_ROOT}/${file}")
        message(FATAL_ERROR "Packaged extension is missing ${file}")
    endif()
endforeach()

file(READ "${OUTPUT_ROOT}/extension/package.json" packagedManifest)
foreach(field IN ITEMS name publisher version)
    string(JSON sourceValue GET "${sourceManifest}" ${field})
    string(JSON packagedValue GET "${packagedManifest}" ${field})
    if(NOT packagedValue STREQUAL sourceValue)
        message(FATAL_ERROR
            "Packaged ${field} '${packagedValue}' does not match "
            "source '${sourceValue}'"
        )
    endif()
endforeach()

foreach(setting IN ITEMS flecha.server.path flecha.compiler.path)
    string(JSON configuredDefault GET "${packagedManifest}"
        contributes configuration properties "${setting}" default)
    if(NOT configuredDefault STREQUAL "")
        message(FATAL_ERROR
            "Packaged ${setting} must use automatic bundle/PATH discovery")
    endif()
endforeach()

file(READ "${OUTPUT_ROOT}/extension/dist/extension.js" packagedClient)
string(FIND "${packagedClient}"
    "FLECHA_BUNDLED_TOOL_LAYOUT" bundledLayoutMarker)
if(bundledLayoutMarker EQUAL -1)
    message(FATAL_ERROR
        "Packaged extension is missing relocatable tool discovery")
endif()

file(READ "${OUTPUT_ROOT}/extension.vsixmanifest" installManifest)
foreach(required IN ITEMS
        "Id=\"${sourceName}\""
        "Publisher=\"${sourcePublisher}\""
        "Version=\"${sourceVersion}\"")
    string(FIND "${installManifest}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "VSIX installation manifest is missing ${required}"
        )
    endif()
endforeach()

file(GLOB_RECURSE packagedFiles
    RELATIVE "${OUTPUT_ROOT}/extension"
    "${OUTPUT_ROOT}/extension/*"
)
foreach(forbidden IN ITEMS
        node_modules
        package-lock.json
        extension.js
        tool-paths.js
        tests)
    foreach(packagedFile IN LISTS packagedFiles)
        if(packagedFile STREQUAL forbidden OR
           packagedFile MATCHES "^${forbidden}/")
            message(FATAL_ERROR
                "VSIX contains development-only path ${packagedFile}"
            )
        endif()
    endforeach()
endforeach()

message(STATUS
    "Verified installable extension ${sourcePublisher}.${sourceName} "
    "${sourceVersion} in ${VSIX}"
)
