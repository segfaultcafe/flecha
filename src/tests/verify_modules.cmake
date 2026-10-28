execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${FLECHA_SOURCE}/main.fl"
        --root "${FLECHA_SOURCE}" --lib "std=${FLECHA_SOURCE}/library" -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "module compilation failed: ${output} ${errors}")
endif()
execute_process(COMMAND "${FLECHA_OUTPUT}" RESULT_VARIABLE result)
if(NOT result EQUAL 42)
    message(FATAL_ERROR "module program returned ${result}, expected 42")
endif()
# Diagnostics from imported sources must identify their file and prevent emission.
file(MAKE_DIRECTORY "${FLECHA_OUTPUT}_invalid")
file(WRITE "${FLECHA_OUTPUT}_invalid/main.fl" "module main; @imports :: root::helper; void main() {}\n")
file(WRITE "${FLECHA_OUTPUT}_invalid/helper.fl" "module helper; export unsafe void leak(own *i32 p) {}\n")
file(REMOVE "${FLECHA_OUTPUT}_invalid/rejected.o")
execute_process(COMMAND "${FLECHA_COMPILER}" --emit-object "${FLECHA_OUTPUT}_invalid/main.fl"
    --root "${FLECHA_OUTPUT}_invalid" -o "${FLECHA_OUTPUT}_invalid/rejected.o"
    RESULT_VARIABLE result ERROR_VARIABLE errors)
if(result EQUAL 0 OR EXISTS "${FLECHA_OUTPUT}_invalid/rejected.o" OR NOT errors MATCHES "helper.fl:FLECHA-S0048")
    message(FATAL_ERROR "imported ownership violation was not diagnosed correctly: ${errors}")
endif()

# Package-node identity must keep same-named generic declarations and their
# specializations distinct through native code generation.
set(graph_root "${FLECHA_OUTPUT}_packages")
file(REMOVE_RECURSE "${graph_root}")
file(MAKE_DIRECTORY "${graph_root}/app" "${graph_root}/first"
    "${graph_root}/second" "${graph_root}/left" "${graph_root}/right"
    "${graph_root}/target")
file(WRITE "${graph_root}/app/main.fl" [=[module main;
@imports :: lib::first::use as first :: lib::second::use as second;
i32 main() { return first::answer() + second::answer(); }
]=])
file(WRITE "${graph_root}/first/use.fl" [=[module use;
@imports :: lib::common::model;
export i32 answer() {
    value := model::make<i32>(20);
    return model::read<i32>(&value);
}
]=])
file(WRITE "${graph_root}/second/use.fl" [=[module use;
@imports :: lib::common::model;
export i32 answer() {
    value := model::make<i32>(22);
    return model::read<i32>(&value);
}
]=])
set(model_source [=[module model;
export structure Box<T> { T value; };
export Box<T> make<T>(T value) {
    result := Box<T>::init(); result.value = value; return result;
}
export T read<T>(&const Box<T> value) { return value->value; }
]=])
file(WRITE "${graph_root}/left/model.fl" "${model_source}")
file(WRITE "${graph_root}/right/model.fl" "${model_source}")
file(WRITE "${graph_root}/target/package-graph.json" [=[{
  "schema": 1,
  "root": "app",
  "packages": [
    {"id":"app", "identity":"local/app", "source":"../app",
     "dependencies":{"first":"first", "second":"second"}},
    {"id":"first", "identity":"owner/first", "source":"../first",
     "dependencies":{"common":"left"}},
    {"id":"second", "identity":"owner/second", "source":"../second",
     "dependencies":{"common":"right"}},
    {"id":"left", "identity":"one/common", "source":"../left",
     "dependencies":{}},
    {"id":"right", "identity":"two/common", "source":"../right",
     "dependencies":{}}
  ]
}
]=])
execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${graph_root}/app/main.fl"
        --package-graph "${graph_root}/target/package-graph.json"
        -o "${FLECHA_OUTPUT}_package_graph"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "package graph compilation failed: ${output} ${errors}")
endif()
execute_process(COMMAND "${FLECHA_OUTPUT}_package_graph" RESULT_VARIABLE result)
if(NOT result EQUAL 42)
    message(FATAL_ERROR "package graph program returned ${result}, expected 42")
endif()
