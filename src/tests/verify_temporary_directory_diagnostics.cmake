if(NOT DEFINED FLECHA_COMPILER OR NOT DEFINED FLECHA_STD OR
   NOT DEFINED FLECHA_OUTPUT)
    message(FATAL_ERROR
        "temporary-directory diagnostic verification requires compiler, std, and output paths")
endif()

file(MAKE_DIRECTORY "${FLECHA_OUTPUT}")

set(source "${FLECHA_OUTPUT}/path_borrow_conflict.fl")
set(object "${FLECHA_OUTPUT}/path_borrow_conflict.o")
file(WRITE "${source}"
    "module path_borrow_conflict;\n"
    "@imports :: lib::std::io::file;\n"
    "void reject() {\n"
    "    temporary, err := file::TemporaryDirectory::create();\n"
    "    if err != none { return; }\n"
    "    borrowedPath := temporary.path();\n"
    "    temporary.remove();\n"
    "    borrowedPath.len();\n"
    "}\n")
file(REMOVE "${object}")

execute_process(
    COMMAND "${FLECHA_COMPILER}" --emit-object "${source}"
        --lib "std=${FLECHA_STD}" -o "${object}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 30)

if(result EQUAL 0 OR EXISTS "${object}" OR
   NOT errors MATCHES "FLECHA-S0029:")
    message(FATAL_ERROR
        "path_borrow_conflict: expected FLECHA-S0029 without object emission:\n"
        "${output}${errors}")
endif()
