file(REMOVE_RECURSE "${FLECHA_OUTPUT}")
file(MAKE_DIRECTORY "${FLECHA_OUTPUT}/src/attached" "${FLECHA_OUTPUT}/src/nested")
file(WRITE "${FLECHA_OUTPUT}/longbow.toml"
    "[package]\nname = \"batching\"\nversion = \"0.1.0\"\nkind = \"library\"\n\n[dependencies]\nstd = { path = \"${FLECHA_STD_PACKAGE}\" }\n")
file(WRITE "${FLECHA_OUTPUT}/src/math.fl" "module math;\n")
file(WRITE "${FLECHA_OUTPUT}/src/a.fl" [=[module math_test;
@imports :: root::math :: lib::std::tests::testing;
test first_failure(&testing::UnitTest t) { t->fail(); }
]=])
file(WRITE "${FLECHA_OUTPUT}/src/b.fl" [=[module math_test;
@imports :: root::math :: lib::std::tests::testing;
test later_file(&testing::UnitTest t) {}
]=])
file(WRITE "${FLECHA_OUTPUT}/src/attached/c.fl" [=[module math_test;
under namespace root;
@imports :: root::math :: lib::std::tests::testing;
test attached_file(&testing::UnitTest t) {}
]=])
file(WRITE "${FLECHA_OUTPUT}/src/nested/math.fl" "module math;\n")
file(WRITE "${FLECHA_OUTPUT}/src/nested/a.fl" [=[module math_test;
@imports :: root::nested::math :: lib::std::tests::testing;
test distinct_namespace(&testing::UnitTest t) {}
]=])
file(WRITE "${FLECHA_OUTPUT}/src/empty.fl" "module empty;\n")
file(WRITE "${FLECHA_OUTPUT}/src/empty_test.fl" [=[module empty_test;
@imports :: root::empty :: lib::std::tests::testing;
]=])
set(trace "${FLECHA_OUTPUT}/compilations.log")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "FLECHA_COMPILER=${FLECHA_PROBE}"
        "FLECHA_REAL_COMPILER=${FLECHA_COMPILER}"
        "FLECHA_COMPILER_TRACE=${trace}"
        "${FLECHA_LONGBOW}" test "${FLECHA_OUTPUT}" --verbose
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error
)
if(NOT status EQUAL 1)
    message(FATAL_ERROR "expected one failing module: ${status}\n${output}${error}")
endif()
foreach(name first_failure later_file attached_file distinct_namespace)
    # Count occurrences independently of the dot leader length.
    string(REGEX MATCHALL "${name} " matches "${output}")
    list(LENGTH matches count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "${name} must run exactly once\n${output}${error}")
    endif()
endforeach()
foreach(expected "FAIL [2/3] test files passed, but [1/3] test files failed"
                 "OK [0/0] tests passed"
                 "FAIL [2/3] test modules passed, but [1/3] test modules failed")
    string(FIND "${output}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "missing ${expected}\n${output}${error}")
    endif()
endforeach()
file(STRINGS "${trace}" invocations)
list(LENGTH invocations count)
if(NOT count EQUAL 3)
    message(FATAL_ERROR "expected three compiler invocations, got ${count}: ${invocations}")
endif()
foreach(invocation IN LISTS invocations)
    if(NOT invocation MATCHES "--whole-module")
        message(FATAL_ERROR "compiler invocation must select the whole module: ${invocation}")
    endif()
endforeach()

# A compilation error in one group must not suppress independent modules.
file(APPEND "${FLECHA_OUTPUT}/src/b.fl" "void invalid() { unknown_name(); }\n")
file(WRITE "${trace}" "")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "FLECHA_COMPILER=${FLECHA_PROBE}"
        "FLECHA_REAL_COMPILER=${FLECHA_COMPILER}"
        "FLECHA_COMPILER_TRACE=${trace}"
        "${FLECHA_LONGBOW}" test "${FLECHA_OUTPUT}" --verbose
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error
)
if(NOT status EQUAL 1 OR NOT error MATCHES "unknown_name" OR
   NOT output MATCHES "distinct_namespace")
    message(FATAL_ERROR "compile failure must preserve later groups\n${output}${error}")
endif()
file(STRINGS "${trace}" invocations)
list(LENGTH invocations count)
if(NOT count EQUAL 3)
    message(FATAL_ERROR "compile failure changed grouping: ${invocations}")
endif()

# An invalid attachment must not be hidden behind a valid group's entry file.
file(WRITE "${FLECHA_OUTPUT}/src/z_bad.fl" [=[module math_test;
under namespace root::nested;
@imports :: root::nested::math :: lib::std::tests::testing;
]=])
file(WRITE "${trace}" "")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "FLECHA_COMPILER=${FLECHA_PROBE}"
        "FLECHA_REAL_COMPILER=${FLECHA_COMPILER}"
        "FLECHA_COMPILER_TRACE=${trace}"
        "${FLECHA_LONGBOW}" test "${FLECHA_OUTPUT}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error
)
file(STRINGS "${trace}" invocations)
list(LENGTH invocations count)
if(NOT status EQUAL 1 OR NOT count EQUAL 4 OR NOT error MATCHES "ancestor")
    message(FATAL_ERROR "invalid attachment was hidden: ${invocations}\n${output}${error}")
endif()
