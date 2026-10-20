set(FLECHA_BUILD_VERSION "" CACHE STRING
    "Version reported by Flecha and Longbow; derived from Git when empty")
option(FLECHA_RELEASE_BUILD
    "Report only PROJECT_VERSION for an official release build" OFF)

if(FLECHA_RELEASE_BUILD)
    set(FLECHA_BUILD_VERSION "${PROJECT_VERSION}")
elseif(FLECHA_BUILD_VERSION STREQUAL "")
    set(FLECHA_BUILD_VERSION "${PROJECT_VERSION}")
    find_package(Git QUIET)
    if(Git_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags
                --match "v${PROJECT_VERSION}" --long --dirty
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            RESULT_VARIABLE describeResult
            OUTPUT_VARIABLE describeOutput
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(describeResult EQUAL 0)
            if(describeOutput MATCHES
               "^v${PROJECT_VERSION}-0-g[0-9a-f]+$")
                set(FLECHA_BUILD_VERSION "${PROJECT_VERSION}")
            else()
                string(REGEX REPLACE "^v" "" FLECHA_BUILD_VERSION
                    "${describeOutput}")
            endif()
        else()
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" describe --always --dirty
                    --abbrev=12
                WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                RESULT_VARIABLE revisionResult
                OUTPUT_VARIABLE revisionOutput
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(revisionResult EQUAL 0 AND
               revisionOutput MATCHES "^([0-9a-f]+)(-dirty)?$")
                set(FLECHA_BUILD_VERSION
                    "${PROJECT_VERSION}-dev-g${revisionOutput}")
            endif()
        endif()
    endif()
endif()

if(NOT FLECHA_BUILD_VERSION MATCHES
   "^[0-9]+\\.[0-9]+\\.[0-9]+(-([0-9]+|dev)-g[0-9a-f]+(-dirty)?)?$")
    message(FATAL_ERROR
        "Invalid FLECHA_BUILD_VERSION: ${FLECHA_BUILD_VERSION}")
endif()

message(STATUS "Flecha build version: ${FLECHA_BUILD_VERSION}")
