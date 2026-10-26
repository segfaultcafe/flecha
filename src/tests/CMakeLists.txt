add_executable(test_flecha
    test_ast.cpp
    test_codegen.cpp
    test_diagnostics.cpp
    test_lowering.cpp
    test_modules.cpp
    test_main.cpp
    test_parser.cpp
    test_runtime.cpp
    test_semantic.cpp
    test_tokenizer.cpp
)

target_link_libraries(test_flecha
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
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_flecha DISCOVERY_MODE PRE_TEST)

add_test(NAME CliTests.FlechaVersion COMMAND $<TARGET_FILE:flecha> --version)
set_tests_properties(CliTests.FlechaVersion PROPERTIES
    PASS_REGULAR_EXPRESSION "^flecha ${PROJECT_VERSION}"
)
add_test(NAME CliTests.LongbowVersion COMMAND $<TARGET_FILE:longbow> --version)
set_tests_properties(CliTests.LongbowVersion PROPERTIES
    PASS_REGULAR_EXPRESSION "^longbow ${PROJECT_VERSION}"
)

add_test(
    NAME LanguageTestTests.PassingModule
    COMMAND $<TARGET_FILE:flecha>
        test
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/language_tests/root/math_test.fl
        --root ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/language_tests/root
        --lib std=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/language_tests/std
)
set_tests_properties(LanguageTestTests.PassingModule PROPERTIES
    PASS_REGULAR_EXPRESSION "math_test.fl [\.]+ OK"
)

add_test(
    NAME LanguageTestTests.PassingModuleVerbose
    COMMAND $<TARGET_FILE:flecha>
        test
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/language_tests/root/math_test.fl
        --root ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/language_tests/root
        --lib std=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/language_tests/std
        --verbose
)
set_tests_properties(LanguageTestTests.PassingModuleVerbose PROPERTIES
    PASS_REGULAR_EXPRESSION "private_access [\.]+ ok"
)

add_test(
    NAME LanguageTestTests.FailingModule
    COMMAND $<TARGET_FILE:flecha>
        test
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/language_tests/root/failure_test.fl
        --root ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/language_tests/root
        --lib std=${PROJECT_SOURCE_DIR}/std/src
)
set_tests_properties(LanguageTestTests.FailingModule PROPERTIES WILL_FAIL TRUE)

add_test(
    NAME LanguageTestTests.FailureDiagnostics
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/language_tests/root/failure_test.fl
        -DFLECHA_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/language_tests/root
        -DFLECHA_STD=${PROJECT_SOURCE_DIR}/std/src
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_test_failure.cmake
)

add_test(
    NAME LanguageTestTests.LongbowDiscovery
    COMMAND ${CMAKE_COMMAND} -E env
        FLECHA_COMPILER=$<TARGET_FILE:flecha>
        $<TARGET_FILE:longbow> test
)
set_tests_properties(LanguageTestTests.LongbowDiscovery PROPERTIES
    WORKING_DIRECTORY
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/longbow_test_project
    PASS_REGULAR_EXPRESSION "OK \\[2/2\\] test files passed"
)

add_test(
    NAME LanguageTestTests.FileLocalSelection
    COMMAND $<TARGET_FILE:flecha>
        test
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/longbow_test_project/src/boundary_cases.fl
        --root ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/longbow_test_project/src
        --lib std=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/longbow_test_project/deps/std/src
        --verbose
)
set_tests_properties(LanguageTestTests.FileLocalSelection PROPERTIES
    PASS_REGULAR_EXPRESSION "aggregates_test_sources [\.]+ ok"
    FAIL_REGULAR_EXPRESSION "discovers_tests"
)

add_test(
    NAME StandardLibraryTests.FlechaModules
    COMMAND ${CMAKE_COMMAND} -E env
        FLECHA_COMPILER=$<TARGET_FILE:flecha>
        FLECHA_STANDARD_LIBRARY=${PROJECT_SOURCE_DIR}/std/src
        $<TARGET_FILE:longbow> test --verbose
)
set_tests_properties(StandardLibraryTests.FlechaModules PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/std
)

add_test(NAME EditorTests.VSCodeManifest
    COMMAND ${CMAKE_COMMAND}
        -DEDITOR_ROOT=${PROJECT_SOURCE_DIR}/editors/vscode
        -DPROJECT_VERSION=${PROJECT_VERSION}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_vscode.cmake
)

add_test(NAME BuildDiagnostics.LLVM
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/missing-llvm-config
        "-DGENERATOR=${CMAKE_GENERATOR}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_llvm_diagnostics.cmake
)

add_test(NAME BuildDiagnostics.Compiler
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_compiler_diagnostics.cmake
)

add_test(NAME BuildDiagnostics.ToolchainConsistency
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_toolchain_consistency.cmake
)

add_test(NAME BuildDiagnostics.WindowsToolchain
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/windows-toolchain
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_windows_toolchain.cmake
)

add_test(
    NAME NativeRuntimeTests.PrintsEveryPrintableType
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/stdout.fl
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/runtime_stdout_test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_stdout.cmake
)

add_test(
    NAME NativeRuntimeTests.StringViewOperations
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/string_view_operations.fl
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/string_view_operations_test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_string_view_operations.cmake
)

add_test(
    NAME NativeRuntimeTests.OwningStrings
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/owning_string.fl
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/owning_string_test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_owning_string.cmake
)

add_test(
    NAME NativeRuntimeTests.OwningStringScalarFormatting
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/owning_string_scalars.fl
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/owning_string_scalars_test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_owning_string_scalars.cmake
)

add_test(
    NAME NativeSemanticTests.DefiniteAssignment
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/definite_assignment.fl
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/definite_assignment_test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_definite_assignment.cmake
)

add_test(
    NAME NativeSemanticTests.ControlFlowDefiniteAssignment
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/control_flow_definite_assignment.fl
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/control_flow_assignment_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_control_flow_assignment.cmake
)

# This fixture shares the control-flow fixture's output/exit contract and
# instrumented-runtime linking, so it can use the same native verifier.
add_test(
    NAME NativeSemanticTests.ConstDeclarations
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/const_declarations.fl
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/const_declarations_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_control_flow_assignment.cmake
)

add_test(
    NAME NativeSemanticTests.UnsafePointers
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/unsafe_pointer_api.fl
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/unsafe_pointer_harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/unsafe_pointers_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_unsafe_pointers.cmake
)

add_test(
    NAME NativeSemanticTests.CheckedReferences
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/checked_reference_api.fl
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/checked_reference_harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/checked_references_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_unsafe_pointers.cmake
)

add_test(
    NAME NativeSemanticTests.FixedArrays
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fixed_array_api.fl
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fixed_array_harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/fixed_arrays_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_fixed_arrays.cmake
)

add_test(
    NAME NativeSemanticTests.Iteration
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/iteration_api.fl
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/iteration_harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/iteration_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_unsafe_pointers.cmake
)

add_test(
    NAME NativeSemanticTests.CheckedSlices
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/checked_slice_api.fl
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/checked_slice_harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/checked_slices_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        "-DFLECHA_TRAP_MODES=r;w;b;o;e;n;f;k;u;m"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_fixed_arrays.cmake
)

add_test(
    NAME NativeSemanticTests.Defer
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/defer_api.fl
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/defer_harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/defer_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        "-DFLECHA_TRAP_MODES=t;a;c"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_fixed_arrays.cmake
)

add_test(
    NAME NativeSemanticTests.Tuples
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tuple_api.fl
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tuple_harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/tuples_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_unsafe_pointers.cmake
)

add_test(
    NAME NativeSemanticTests.ErrorValues
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/error_values_api.fl
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/error_values_harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/error_values_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_unsafe_pointers.cmake
)

add_test(
    NAME NativeSemanticTests.Structures
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/structure_api.fl
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/structure_harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/structures_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_unsafe_pointers.cmake
)

add_test(
    NAME NativeSemanticTests.RawAllocation
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/allocation_api.fl
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/allocation_harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/allocation_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -DFLECHA_LINK_OPTIONS=-Wl,--wrap=flecha_runtime_v1_alloc
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_unsafe_pointers.cmake
)

add_test(
    NAME CompilerDiagnostics.MemoryOwnership
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/memory_diagnostics.fl
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/rejected_memory.o
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_memory_diagnostics.cmake
)

add_test(NAME NativeSemanticTests.Modules
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/modules
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/modules_test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_modules.cmake)

add_test(NAME NativeSemanticTests.CallableIntegration
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/callable_integration
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/callable_integration_test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_callable_integration.cmake)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME NativeSemanticTests.CallableAllocationFailure
        COMMAND ${CMAKE_COMMAND}
            -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
            -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/callable_allocation_failure.fl
            -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/callable_allocation_failure_harness.cpp
            -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/callable_allocation_failure_test
            -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
            -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
            -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_callable_allocation_failure.cmake)
endif()

add_test(NAME LongbowTests.Projects
    COMMAND ${CMAKE_COMMAND}
        -DLONGBOW=$<TARGET_FILE:longbow>
        -DFLECHA_STD=${PROJECT_SOURCE_DIR}/std/src
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/longbow_projects
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_longbow.cmake)

add_test(NAME NativeExternTests.CAbiAndLinking
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DLONGBOW=$<TARGET_FILE:longbow>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/extern_link_test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_extern.cmake)

add_test(NAME StandardLibraryTests.FilesystemReadFile
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_STD=${PROJECT_SOURCE_DIR}/std/src
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/filesystem_test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_filesystem.cmake)

add_test(NAME StandardLibraryTests.FileStream
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_STD=${PROJECT_SOURCE_DIR}/std/src
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/file_stream_test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_file_stream.cmake)

add_test(NAME NativeSemanticTests.RuntimeArrays
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/runtime_array_api.fl
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/runtime_array_harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/runtime_array_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -DFLECHA_LINK_OPTIONS=-Wl,--wrap=flecha_runtime_v1_alloc_array
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_unsafe_pointers.cmake)

add_test(NAME StandardLibraryTests.RawArray
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dynamic_array/main.fl
        -DFLECHA_STD=${PROJECT_SOURCE_DIR}/std/src
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dynamic_array/harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/dynamic_array_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_dynamic_array.cmake)

add_test(NAME NativeSemanticTests.Generics
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/generics_api.fl
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/generics_harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generics_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_unsafe_pointers.cmake)

add_test(NAME NativeSemanticTests.GenericModules
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/generics
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generic_modules_test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_modules.cmake)

add_test(NAME CompilerDiagnostics.Generics
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generic_diagnostics
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_generic_diagnostics.cmake)

add_test(NAME StandardLibraryTests.RawArrayScalarTypes
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dynamic_array/scalars.fl
        -DFLECHA_STD=${PROJECT_SOURCE_DIR}/std/src
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dynamic_array/harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/array_scalar_types_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -DFLECHA_ARRAY_FAIL_CALL=0
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_dynamic_array.cmake)

add_test(NAME StandardLibraryTests.RawArrayStructures
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dynamic_array/structures.fl
        -DFLECHA_STD=${PROJECT_SOURCE_DIR}/std/src
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dynamic_array/harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/array_structure_types_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -DFLECHA_ARRAY_FAIL_CALL=0
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_dynamic_array.cmake)

add_test(NAME NativeSemanticTests.NoneComparisons
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/none_comparisons_api.fl
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/none_comparisons_harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/none_comparisons_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_unsafe_pointers.cmake)

add_test(NAME NativeSemanticTests.StructureZero
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/structure_zero_api.fl
        -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/structure_zero_harness.cpp
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/structure_zero_test
        -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
        -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
        -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_unsafe_pointers.cmake)

# The safe fixtures exercise the public API without unsafe caller blocks.
foreach(case IN ITEMS
        main
        scalars
        structures
        lifecycle
        from_slice
        append
        clone
        remove
        swap_remove
        structure_mutations
        algorithms
        own_capture)
    set(array_fail_call 0)
    if(case STREQUAL "main")
        set(array_fail_call 2)
    endif()
    add_test(NAME StandardLibraryTests.SafeArray.${case}
        COMMAND ${CMAKE_COMMAND}
            -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
            -DFLECHA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dynamic_array/safe/${case}.fl
            -DFLECHA_STD=${PROJECT_SOURCE_DIR}/std/src
            -DFLECHA_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dynamic_array/harness.cpp
            -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/safe_array_${case}
            -DFLECHA_RUNTIME=$<TARGET_FILE:runtime>
            -DFLECHA_LINKER=${CMAKE_CXX_COMPILER}
            -DFLECHA_SANITIZERS=${FLECHA_ENABLE_SANITIZERS}
            -DFLECHA_ARRAY_FAIL_CALL=${array_fail_call}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_dynamic_array.cmake)
endforeach()

add_test(NAME CompilerDiagnostics.SafeArray
    COMMAND ${CMAKE_COMMAND}
        -DFLECHA_COMPILER=$<TARGET_FILE:flecha>
        -DFLECHA_STD=${PROJECT_SOURCE_DIR}/std/src
        -DFLECHA_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/safe_array_diagnostics
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_safe_array_diagnostics.cmake)
