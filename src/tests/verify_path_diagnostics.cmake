file(MAKE_DIRECTORY "${FLECHA_OUTPUT}")

function(reject_path name code body)
    set(source "${FLECHA_OUTPUT}/${name}.fl")
    set(object "${FLECHA_OUTPUT}/${name}.o")
    file(WRITE "${source}"
        "module ${name};\n"
        "@imports :: lib::std::io::path;\n"
        "${body}\n")
    file(REMOVE "${object}")
    execute_process(
        COMMAND "${FLECHA_COMPILER}" --emit-object "${source}"
            --lib "std=${FLECHA_STD}" -o "${object}"
        RESULT_VARIABLE result ERROR_VARIABLE errors TIMEOUT 30)
    if(result EQUAL 0 OR EXISTS "${object}" OR NOT errors MATCHES "${code}")
        message(FATAL_ERROR
            "${name}: expected ${code} without object emission: ${errors}")
    endif()
endfunction()

reject_path(path_view_source_replacement FLECHA-S0029 [=[
void main() {
    value, failure := path::normalize("alpha/name.txt");
    name := path::file_name(value.view());
    value = none;
    stdout(name);
}
]=])

reject_path(path_view_local_escape FLECHA-S0058 [=[
stringv make_extension() {
    value, failure := path::normalize("alpha/name.txt");
    return path::extension(value.view());
}
]=])
