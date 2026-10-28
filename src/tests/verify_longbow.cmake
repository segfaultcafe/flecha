file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
function(run expected directory)
    execute_process(COMMAND "${LONGBOW}" ${ARGN} WORKING_DIRECTORY "${directory}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT result EQUAL expected)
        message(FATAL_ERROR "longbow ${ARGN} returned ${result}, expected ${expected}: ${output} ${errors}")
    endif()
    set(last_output "${output}" PARENT_SCOPE)
    set(last_errors "${errors}" PARENT_SCOPE)
endfunction()
function(run_with_standard_library expected directory)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "FLECHA_STANDARD_LIBRARY=${FLECHA_STD}" "${LONGBOW}" ${ARGN}
        WORKING_DIRECTORY "${directory}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT result EQUAL expected)
        message(FATAL_ERROR
            "longbow ${ARGN} returned ${result}, expected ${expected}: ${output} ${errors}")
    endif()
    set(last_output "${output}" PARENT_SCOPE)
    set(last_errors "${errors}" PARENT_SCOPE)
endfunction()
function(run_cached expected directory)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "LONGBOW_CACHE_DIR=${TEST_ROOT}/cache" "${LONGBOW}" ${ARGN}
        WORKING_DIRECTORY "${directory}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT result EQUAL expected)
        message(FATAL_ERROR
            "cached longbow ${ARGN} returned ${result}, expected ${expected}: ${output} ${errors}")
    endif()
    set(last_output "${output}" PARENT_SCOPE)
    set(last_errors "${errors}" PARENT_SCOPE)
endfunction()
run(0 "${TEST_ROOT}" new hello)
if(NOT EXISTS "${TEST_ROOT}/hello/longbow.toml" OR NOT EXISTS "${TEST_ROOT}/hello/.gitignore")
    message(FATAL_ERROR "missing scaffold files")
endif()
run(0 "${TEST_ROOT}/hello" check)
if(EXISTS "${TEST_ROOT}/hello/target")
    message(FATAL_ERROR "check must not create build artifacts")
endif()
run(0 "${TEST_ROOT}/hello" run)
if(NOT last_output STREQUAL "Hello from Flecha!\n")
    message(FATAL_ERROR "wrong program output: ${last_output}")
endif()
file(WRITE "${TEST_ROOT}/hello/src/main.fl"
    "module main;\n"
    "@imports :: lib::std::containers::collections;\n"
    "void main() { stdoutln(\"Hello from Flecha!\"); }\n")
run_with_standard_library(0 "${TEST_ROOT}/hello" run)
if(NOT last_output STREQUAL "Hello from Flecha!\n")
    message(FATAL_ERROR "wrong program output with standard library: ${last_output}")
endif()
run_with_standard_library(0 "${TEST_ROOT}/hello/src" run --
    "argument with spaces" "$(not-a-shell-command)")
file(SHA256 "${TEST_ROOT}/hello/src/main.fl" before)
run(1 "${TEST_ROOT}" new hello)
run(1 "${TEST_ROOT}/hello" init)
file(SHA256 "${TEST_ROOT}/hello/src/main.fl" after)
if(NOT before STREQUAL after)
    message(FATAL_ERROR "existing program was overwritten")
endif()
run(1 "${TEST_ROOT}" new "bad?name")
run(0 "${TEST_ROOT}" new "unsafe" --lib)
if(NOT EXISTS "${TEST_ROOT}/unsafe/src/unsafe_project.fl")
    message(FATAL_ERROR "keyword library did not normalize its module name")
endif()
run(1 "${TEST_ROOT}/hello" check --unknown)
run(0 "${TEST_ROOT}" new "test-me")
run(0 "${TEST_ROOT}" build "test-me")
if(NOT EXISTS "${TEST_ROOT}/test-me/target/test-me${EXECUTABLE_SUFFIX}")
    message(FATAL_ERROR "hyphenated project did not produce its named artifact")
endif()
run(0 "${TEST_ROOT}" new "hi mom")
run(0 "${TEST_ROOT}" build "hi mom")
if(NOT EXISTS "${TEST_ROOT}/hi mom/target/hi mom${EXECUTABLE_SUFFIX}")
    message(FATAL_ERROR "spaced project did not produce its named artifact")
endif()
run(0 "${TEST_ROOT}" new "math-tools" --lib)
if(NOT EXISTS "${TEST_ROOT}/math-tools/src/math_tools.fl")
    message(FATAL_ERROR "hyphenated library did not normalize its module name")
endif()
run(0 "${TEST_ROOT}" build "math-tools")
run(0 "${TEST_ROOT}" new numbers --lib)
run(0 "${TEST_ROOT}/numbers" build)
run(1 "${TEST_ROOT}/numbers" run)
file(APPEND "${TEST_ROOT}/hello/longbow.toml" "numbers = { path = \"../numbers\" }\n")
file(WRITE "${TEST_ROOT}/hello/src/main.fl" "module main; @imports :: lib::numbers::numbers; i32 main() { return numbers::answer(); }\n")
run(42 "${TEST_ROOT}/hello" run)
# Recursive path dependencies are resolved into a package-local alias graph.
run(0 "${TEST_ROOT}" new leaf --lib)
run(0 "${TEST_ROOT}" new middle --lib)
file(APPEND "${TEST_ROOT}/middle/longbow.toml"
    "leaf = { path = \"../leaf\" }\n")
file(WRITE "${TEST_ROOT}/middle/src/middle.fl"
    "module middle; @imports :: lib::leaf::leaf; "
    "export i32 answer() { return leaf::answer(); }\n")
run(0 "${TEST_ROOT}" new recursive)
file(APPEND "${TEST_ROOT}/recursive/longbow.toml"
    "middle = { path = \"../middle\" }\n")
file(WRITE "${TEST_ROOT}/recursive/src/main.fl"
    "module main; @imports :: lib::middle::middle; "
    "i32 main() { return middle::answer(); }\n")
run(42 "${TEST_ROOT}/recursive" run)
if(NOT EXISTS "${TEST_ROOT}/recursive/target/package-graph.json")
    message(FATAL_ERROR "Longbow did not generate the compiler package graph")
endif()
if(NOT EXISTS "${TEST_ROOT}/recursive/longbow.lock")
    message(FATAL_ERROR "Longbow did not generate longbow.lock")
endif()
file(SHA256 "${TEST_ROOT}/recursive/longbow.lock" lock_before)
run(42 "${TEST_ROOT}/recursive" run)
file(SHA256 "${TEST_ROOT}/recursive/longbow.lock" lock_after)
if(NOT lock_before STREQUAL lock_after)
    message(FATAL_ERROR "unchanged resolution produced a nondeterministic lockfile")
endif()
# Pinned Git dependencies are exported without checkout, verified into the
# content-addressed cache, and remain buildable after the origin disappears.
find_program(GIT_EXECUTABLE git REQUIRED)
run(0 "${TEST_ROOT}" new git-leaf --lib)
execute_process(COMMAND "${GIT_EXECUTABLE}" init -q
    WORKING_DIRECTORY "${TEST_ROOT}/git-leaf" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${GIT_EXECUTABLE}" config user.name "Flecha Tests"
    WORKING_DIRECTORY "${TEST_ROOT}/git-leaf" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${GIT_EXECUTABLE}" config user.email "tests@flecha.invalid"
    WORKING_DIRECTORY "${TEST_ROOT}/git-leaf" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${GIT_EXECUTABLE}" add longbow.toml src/git_leaf.fl
    WORKING_DIRECTORY "${TEST_ROOT}/git-leaf" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${GIT_EXECUTABLE}" commit -q -m "fixture"
    WORKING_DIRECTORY "${TEST_ROOT}/git-leaf" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${TEST_ROOT}/git-leaf" OUTPUT_VARIABLE git_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
run(0 "${TEST_ROOT}" new git-app)
file(APPEND "${TEST_ROOT}/git-app/longbow.toml"
    "leaf = { git = \"../git-leaf\", rev = \"${git_revision}\" }\n")
file(WRITE "${TEST_ROOT}/git-app/src/main.fl"
    "module main; @imports :: lib::leaf::git_leaf; "
    "i32 main() { return git_leaf::answer(); }\n")
run_cached(42 "${TEST_ROOT}/git-app" run)
file(READ "${TEST_ROOT}/git-app/longbow.lock" git_lock)
if(NOT git_lock MATCHES "\"kind\": \"git\"" OR
   NOT git_lock MATCHES "\"revision\": \"${git_revision}\"" OR
   NOT git_lock MATCHES "\"checksum\": \"sha256:[0-9a-f]+\"")
    message(FATAL_ERROR "Git resolution metadata is missing from the lock: ${git_lock}")
endif()
# Corrupt content is never reused. With the origin online, Longbow reacquires
# and repairs the entry before the offline proof below.
file(GLOB cached_git_sources "${TEST_ROOT}/cache/sources/sha256/*/source")
list(LENGTH cached_git_sources cached_git_source_count)
if(NOT cached_git_source_count EQUAL 1)
    message(FATAL_ERROR "expected one cached Git source, found ${cached_git_source_count}")
endif()
list(GET cached_git_sources 0 cached_git_source)
file(WRITE "${cached_git_source}/src/git_leaf.fl" "corrupt\n")
run_cached(42 "${TEST_ROOT}/git-app" run)
file(REMOVE_RECURSE "${TEST_ROOT}/git-leaf")
run_cached(42 "${TEST_ROOT}/git-app" run)
# Package compatibility is diagnosed during resolution, before compiler work.
run(0 "${TEST_ROOT}" new incompatible)
file(READ "${TEST_ROOT}/incompatible/longbow.toml" incompatible_manifest)
string(REPLACE "kind = \"program\""
    "kind = \"program\"\nflecha = \">=99.0.0\""
    incompatible_manifest "${incompatible_manifest}")
file(WRITE "${TEST_ROOT}/incompatible/longbow.toml" "${incompatible_manifest}")
run(1 "${TEST_ROOT}/incompatible" check)
if(NOT last_errors MATCHES "requires Flecha >=99.0.0")
    message(FATAL_ERROR "incompatible Flecha requirement was not diagnosed: ${last_errors}")
endif()
# Resolver cycles are diagnosed before invoking the compiler.
file(APPEND "${TEST_ROOT}/leaf/longbow.toml"
    "middle = { path = \"../middle\" }\n")
run(1 "${TEST_ROOT}/recursive" check)
if(NOT last_errors MATCHES "package dependency cycle")
    message(FATAL_ERROR "path dependency cycle was not diagnosed: ${last_errors}")
endif()
# Build errors must stop run, rather than execute the previous artifact.
file(WRITE "${TEST_ROOT}/hello/src/main.fl" "module main; i32 main() { return missing; }\n")
run(1 "${TEST_ROOT}/hello" run)
run(0 "${TEST_ROOT}/hello" clean)
if(EXISTS "${TEST_ROOT}/hello/target" OR NOT EXISTS "${TEST_ROOT}/hello/src/main.fl")
    message(FATAL_ERROR "clean removed source or left artifacts")
endif()
file(MAKE_DIRECTORY "${TEST_ROOT}/initialized")
file(WRITE "${TEST_ROOT}/initialized/.gitignore" "keep-me\n")
run(0 "${TEST_ROOT}/initialized" init)
file(READ "${TEST_ROOT}/initialized/.gitignore" ignore)
if(NOT ignore STREQUAL "keep-me\n")
    message(FATAL_ERROR "init overwrote .gitignore")
endif()
run(0 "${TEST_ROOT}/initialized" run)
# Never follow a user-provided target symlink when building or cleaning.
file(MAKE_DIRECTORY "${TEST_ROOT}/outside")
file(WRITE "${TEST_ROOT}/outside/precious" "preserve\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink "${TEST_ROOT}/outside" "${TEST_ROOT}/hello/target")
run(1 "${TEST_ROOT}/hello" clean)
run(1 "${TEST_ROOT}/hello" build)
if(NOT EXISTS "${TEST_ROOT}/outside/precious")
    message(FATAL_ERROR "clean followed target symlink")
endif()
