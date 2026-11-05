foreach(variable IN ITEMS LONGBOW FLECHA_COMPILER FLECHA_STD FIXTURES TEST_ROOT)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/packages")
file(COPY "${FIXTURES}/leaf" DESTINATION "${TEST_ROOT}/packages")
file(COPY "${FIXTURES}/package" DESTINATION "${TEST_ROOT}/packages")
file(COPY "${FIXTURES}/consumer" DESTINATION "${TEST_ROOT}/packages")

set(registry "${TEST_ROOT}/registry")
set(cache "${TEST_ROOT}/cache")
set(leaf "${TEST_ROOT}/packages/leaf")
set(package "${TEST_ROOT}/packages/package")
set(consumer "${TEST_ROOT}/packages/consumer")

function(run_registry expected directory)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "FLECHA_COMPILER=${FLECHA_COMPILER}"
            "FLECHA_STANDARD_LIBRARY=${FLECHA_STD}"
            "LONGBOW_CACHE_DIR=${cache}"
            "LONGBOW_REGISTRY=file://${registry}"
            "LONGBOW_TOKEN=community-fixture-token"
            "${LONGBOW}" ${ARGN}
        WORKING_DIRECTORY "${directory}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    if(NOT result EQUAL expected)
        message(FATAL_ERROR
            "longbow ${ARGN} returned ${result}, expected ${expected}: "
            "${output}${errors}")
    endif()
    set(last_output "${output}" PARENT_SCOPE)
    set(last_errors "${errors}" PARENT_SCOPE)
endfunction()

# The checked source must continue to normalize to the immutable public
# archives referenced by consumer/longbow.lock.
run_registry(0 "${leaf}" publish --dry-run)
if(EXISTS "${registry}")
    message(FATAL_ERROR "publish --dry-run mutated the registry")
endif()
if(NOT last_output MATCHES
   "segfaultcafe/e2e-community-leaf@0.1.0" OR
   NOT last_output MATCHES
   "sha256:c0485516a85014b05027a8880c1544f739a4092523360c6b99b5e9ffaf16ea88")
    message(FATAL_ERROR "leaf dry-run archive changed: ${last_output}")
endif()
run_registry(0 "${leaf}" publish)

run_registry(0 "${package}" publish --dry-run)
if(NOT last_output MATCHES
   "segfaultcafe/e2e-community-package@0.1.0" OR
   NOT last_output MATCHES
   "sha256:ba7b48d07cee4cdbf1a0e37f9efd8b2b7474f90a8e0d16c4312c60f1174f6e7f")
    message(FATAL_ERROR "package dry-run archive changed: ${last_output}")
endif()
run_registry(0 "${package}" publish)

# The portable public lock is accepted against the same local protocol and is
# rewritten deterministically for this registry location.
run_registry(0 "${consumer}" fetch)
run_registry(42 "${consumer}" run)
file(READ "${consumer}/longbow.lock" lock)
file(READ "${consumer}/target/package-graph.json" graph)
foreach(required IN ITEMS
        "segfaultcafe/e2e-community-package"
        "segfaultcafe/e2e-community-leaf"
        "\"community\": \"registry-1\""
        "\"foundation\": \"registry-2\"")
    if(NOT lock MATCHES "${required}" AND NOT graph MATCHES "${required}")
        message(FATAL_ERROR
            "transitive package graph is missing ${required}: ${lock}${graph}")
    endif()
endforeach()
file(SHA256 "${consumer}/longbow.lock" lock_before)
run_registry(42 "${consumer}" run)
file(SHA256 "${consumer}/longbow.lock" lock_after)
if(NOT lock_before STREQUAL lock_after)
    message(FATAL_ERROR "unchanged resolution produced a nondeterministic lock")
endif()

run_registry(0 "${package}" yank 0.1.0)
run_registry(0 "${leaf}" yank 0.1.0)

# Existing locks retain yanked versions, while an unlocked consumer cannot
# select them.
run_registry(42 "${consumer}" run)
file(COPY "${consumer}" DESTINATION "${TEST_ROOT}/unlocked")
file(REMOVE "${TEST_ROOT}/unlocked/consumer/longbow.lock")
file(REMOVE_RECURSE "${TEST_ROOT}/unlocked/consumer/target")
run_registry(1 "${TEST_ROOT}/unlocked/consumer" check)
if(NOT last_errors MATCHES "no active version")
    message(FATAL_ERROR
        "new resolution selected a yanked version: ${last_errors}")
endif()

# Removing the registry proves that the locked graph rebuilds entirely from
# verified indexes and content-addressed archives in the cache.
file(REMOVE_RECURSE "${registry}" "${consumer}/target")
run_registry(42 "${consumer}" run)
