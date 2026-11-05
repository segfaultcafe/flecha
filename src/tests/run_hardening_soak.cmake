cmake_minimum_required(VERSION 3.20)

foreach(required IN ITEMS
        FLECHA_TEST_BINARY FLECHA_CTEST FLECHA_BUILD_DIR
        FLECHA_OUTPUT FLECHA_PROFILE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(LOCK "${FLECHA_BUILD_DIR}/hardening-soak.lock"
    GUARD PROCESS
    TIMEOUT 0
    RESULT_VARIABLE lock_result)
if(NOT lock_result STREQUAL "0")
    message(FATAL_ERROR
        "another hardening soak is using ${FLECHA_BUILD_DIR}: ${lock_result}")
endif()

set(seed 15215320261105)
if(FLECHA_PROFILE STREQUAL "fast")
    set(runtime_repeats 2)
    set(stateful_rounds 1)
    set(include_network FALSE)
elseif(FLECHA_PROFILE STREQUAL "sanitizer")
    set(runtime_repeats 5)
    set(stateful_rounds 1)
    set(include_network TRUE)
elseif(FLECHA_PROFILE STREQUAL "campaign")
    set(runtime_repeats 20)
    set(stateful_rounds 3)
    set(include_network TRUE)
else()
    message(FATAL_ERROR "unknown hardening profile '${FLECHA_PROFILE}'")
endif()
if(DEFINED FLECHA_DISABLE_NETWORK AND FLECHA_DISABLE_NETWORK)
    set(include_network FALSE)
endif()

file(REMOVE_RECURSE "${FLECHA_OUTPUT}")
file(MAKE_DIRECTORY "${FLECHA_OUTPUT}/tmp")
set(log "${FLECHA_OUTPUT}/hardening-soak.log")

set(runtime_filter
    "RuntimeChildProcessTests.*:RuntimeEnvironmentTests.*:RuntimeRandomTests.*:RuntimeSecurityTests.*:RuntimeTimeTests.ReturnsNormalizedClockObservations:RuntimeTimeTests.ClearsOutputsWhenPointersAreInvalid:RuntimeFilesystemTests.*:RuntimeFilesystemMutationTests.*:RuntimeFileStreamTests.*:LongbowCacheTests.*:LongbowArchiveTests.*:LongbowRegistryTests.*:LongbowResolverTests.*")
if(include_network)
    string(APPEND runtime_filter
        ":RuntimeNetworkTests.*:RuntimeTcpTests.*:RuntimeUdpTests.*")
endif()

if(include_network)
    set(network_json true)
else()
    set(network_json false)
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "TMPDIR=${FLECHA_OUTPUT}/tmp"
        "TMP=${FLECHA_OUTPUT}/tmp"
        "TEMP=${FLECHA_OUTPUT}/tmp"
        "${FLECHA_TEST_BINARY}"
        "--gtest_filter=${runtime_filter}"
        "--gtest_repeat=${runtime_repeats}"
        --gtest_shuffle
        "--gtest_random_seed=15215"
        --gtest_brief=1
    WORKING_DIRECTORY "${FLECHA_BUILD_DIR}"
    RESULT_VARIABLE runtime_result
    OUTPUT_VARIABLE runtime_output
    ERROR_VARIABLE runtime_error
    TIMEOUT 900
)
file(APPEND "${log}" "[runtime]\n${runtime_output}${runtime_error}\n")
if(NOT runtime_result STREQUAL "0")
    message(FATAL_ERROR
        "repeated runtime campaign failed (${runtime_result}); see ${log}")
endif()

set(stateful_tests
    RuntimeFilesystemMutationTests.PreservesPendingEntryOnAllocationFailure
    RuntimeFileStreamTests.CleansUpTemporaryFilesOnAllocationFailure
    RuntimeFilesystemMutationTests.CleansUpTemporaryDirectoriesOnAllocationFailure
    RuntimeEnvironmentTests.PreservesStateOnAllocationFailure
    CrashableFunctionTests.StringBuilderAllocationFailure
    StandardLibraryTests.AssertionAllocationFailure
    StandardLibraryTests.TestSupportAllocationFailure
    StandardLibraryTests.BenchmarkAllocationFailure
    NativeSemanticTests.CallableAllocationFailure
    StandardLibraryTests.FilesystemCopyAllocationFailure
    StandardLibraryTests.ProcessArgumentsAllocationFailure
    StandardLibraryTests.ChildProcessAllocationFailure.2
    StandardLibraryTests.ChildProcessAllocationFailure.3
    StandardLibraryTests.EnvironmentAllocationFailure
    StandardLibraryTests.BufferedIo.reader_allocation_failure
    StandardLibraryTests.BufferedIo.writer_allocation_failure
    StandardLibraryTests.Path.join_allocation_failure
    StandardLibraryTests.Path.normalize_allocation_failure_1
    StandardLibraryTests.Path.normalize_allocation_failure_2
    LongbowTests.Projects
    LongbowTests.CommunityPackageLifecycle
    LongbowTests.WorkspaceGuardNorthStar
    LongbowTests.LedgerNorthStar
    StandardLibraryTests.ChildProcess
    StandardLibraryTests.SafeArray.move_only_initial_failure
    StandardLibraryTests.SafeArray.move_only_push
    StandardLibraryTests.SafeArray.lifecycle
    StandardLibraryTests.SafeArray.algorithms
    StandardLibraryTests.SafeArray.algorithms_move_only
    StandardLibraryTests.SafeArray.nested_ownership
    StandardLibraryTests.SafeArray.borrow_views
    StandardLibraryTests.SafeDeque.move_only_initial_failure
    StandardLibraryTests.SafeDeque.move_only_growth_failure
    StandardLibraryTests.SafeDeque.move_only_front_growth_failure
    StandardLibraryTests.SafeDeque.move_only_lifecycle
    StandardLibraryTests.SafeDeque.nested_ownership
    StandardLibraryTests.SafeStack.move_only_initial_failure
    StandardLibraryTests.SafeStack.move_only_growth_failure
    StandardLibraryTests.SafeStack.move_only_lifecycle
    StandardLibraryTests.SafeStack.nested_ownership
    StandardLibraryTests.SafeHashMap.initial_state_failure
    StandardLibraryTests.SafeHashMap.initial_key_failure
    StandardLibraryTests.SafeHashMap.initial_value_failure
    StandardLibraryTests.SafeHashMap.growth_state_failure
    StandardLibraryTests.SafeHashMap.growth_key_failure
    StandardLibraryTests.SafeHashMap.growth_value_failure
    StandardLibraryTests.SafeHashMap.move_only_lifecycle
    StandardLibraryTests.SafeHashMap.nested_ownership
    StandardLibraryTests.SafeHashSet.initial_state_failure
    StandardLibraryTests.SafeHashSet.initial_key_failure
    StandardLibraryTests.SafeHashSet.initial_marker_failure
    StandardLibraryTests.SafeHashSet.growth_state_failure
    StandardLibraryTests.SafeHashSet.growth_key_failure
    StandardLibraryTests.SafeHashSet.growth_marker_failure
    StandardLibraryTests.SafeHashSet.move_only_lifecycle
    StandardLibraryTests.SafeHashSet.nested_ownership
    StandardLibraryTests.SafeBinaryHeap.move_only_initial_failure
    StandardLibraryTests.SafeBinaryHeap.move_only_growth_failure
    StandardLibraryTests.SafeBinaryHeap.move_only_lifecycle
    StandardLibraryTests.SafeBinaryHeap.nested_ownership
    StandardLibraryTests.SafeQueue.move_only_initial_failure
    StandardLibraryTests.SafeQueue.move_only_growth_failure
    StandardLibraryTests.SafeQueue.move_only_lifecycle
    StandardLibraryTests.SafeQueue.nested_ownership
)
if(include_network)
    list(APPEND stateful_tests
        RuntimeUdpTests.ClosesNativeSocketOnAllocationFailure
        StandardLibraryTests.NetworkAddressAllocationFailure
        StandardLibraryTests.NetworkSocketAddressAllocationFailure.1
        StandardLibraryTests.NetworkSocketAddressAllocationFailure.2
        StandardLibraryTests.NetworkAddressCrashableAllocationFailure
        StandardLibraryTests.NetworkIpv6AddressCrashableAllocationFailure
        StandardLibraryTests.NetworkIpv6SocketAddressCrashableAllocationFailure
        StandardLibraryTests.NetworkSocketAddressCrashableAllocationFailure.1
        StandardLibraryTests.NetworkSocketAddressCrashableAllocationFailure.2
    )
endif()

list(LENGTH stateful_tests stateful_test_count)
math(EXPR last_round "${stateful_rounds} - 1")
foreach(round RANGE 0 ${last_round})
    set(round_tests ${stateful_tests})
    math(EXPR reverse_round "${round} % 2")
    if(reverse_round EQUAL 1)
        list(REVERSE round_tests)
    endif()
    foreach(test_name IN LISTS round_tests)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env
                "TMPDIR=${FLECHA_OUTPUT}/tmp"
                "TMP=${FLECHA_OUTPUT}/tmp"
                "TEMP=${FLECHA_OUTPUT}/tmp"
                "${FLECHA_CTEST}" --test-dir "${FLECHA_BUILD_DIR}"
                --output-on-failure -R "^${test_name}$"
            RESULT_VARIABLE test_result
            OUTPUT_VARIABLE test_output
            ERROR_VARIABLE test_error
            TIMEOUT 180
        )
        file(APPEND "${log}"
            "[round ${round}] ${test_name}\n${test_output}${test_error}\n")
        if(NOT test_result STREQUAL "0")
            message(FATAL_ERROR
                "stateful test ${test_name} failed in round ${round} "
                "(${test_result}); see ${log}\n${test_output}${test_error}")
        endif()
    endforeach()
endforeach()

file(GLOB_RECURSE temporary_leftovers
    LIST_DIRECTORIES TRUE "${FLECHA_OUTPUT}/tmp/*")
if(temporary_leftovers)
    list(JOIN temporary_leftovers "\n  " leftover_list)
    message(FATAL_ERROR
        "hardening campaign accumulated temporary state:\n  ${leftover_list}")
endif()

file(WRITE "${FLECHA_OUTPUT}/hardening-soak-report.json"
    "{\n"
    "  \"format\": \"flecha-hardening-soak-v1\",\n"
    "  \"profile\": \"${FLECHA_PROFILE}\",\n"
    "  \"seed\": ${seed},\n"
    "  \"runtime_repetitions\": ${runtime_repeats},\n"
    "  \"stateful_rounds\": ${stateful_rounds},\n"
    "  \"stateful_tests_per_round\": ${stateful_test_count},\n"
    "  \"network_enabled\": ${network_json}\n"
    "}\n")

message(STATUS
    "hardening soak ${FLECHA_PROFILE} passed with seed ${seed}")
