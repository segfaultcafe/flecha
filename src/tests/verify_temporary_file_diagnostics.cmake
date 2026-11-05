if(NOT DEFINED FLECHA_COMPILER OR NOT DEFINED FLECHA_STD OR
   NOT DEFINED FLECHA_OUTPUT)
    message(FATAL_ERROR
        "temporary-file diagnostic verification requires compiler, std, and output paths")
endif()

file(MAKE_DIRECTORY "${FLECHA_OUTPUT}")

function(reject_temporary_file_access name body)
    set(source "${FLECHA_OUTPUT}/${name}.fl")
    set(object "${FLECHA_OUTPUT}/${name}.o")
    file(WRITE "${source}"
        "module ${name};\n"
        "@imports :: lib::std::io::file;\n"
        "void reject() {\n"
        "    temporary, err := file::TemporaryFile::create();\n"
        "    if err != none { return; }\n"
        "${body}\n"
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
            "${name}: expected FLECHA-S0029 without object emission:\n"
            "${output}${errors}")
    endif()
endfunction()

reject_temporary_file_access(file_borrow_conflict [=[
    handle := temporary.file();
    temporary.remove();
    handle->isOpen();
]=])

reject_temporary_file_access(path_borrow_conflict [=[
    borrowedPath := temporary.path();
    temporary.remove();
    borrowedPath.len();
]=])
