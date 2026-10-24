file(MAKE_DIRECTORY "${FLECHA_OUTPUT}")

function(reject_array name code body)
    set(source "${FLECHA_OUTPUT}/${name}.fl")
    set(object "${FLECHA_OUTPUT}/${name}.o")
    file(WRITE "${source}" "module ${name};\n@imports :: lib::std::containers::array;\n${body}\n")
    file(REMOVE "${object}")
    execute_process(
        COMMAND "${FLECHA_COMPILER}" --emit-object "${source}"
            --lib "std=${FLECHA_STD}" -o "${object}"
        RESULT_VARIABLE result ERROR_VARIABLE errors TIMEOUT 30)
    if(result EQUAL 0 OR EXISTS "${object}" OR NOT errors MATCHES "${code}")
        message(FATAL_ERROR "${name}: expected ${code} without object emission: ${errors}")
    endif()
endfunction()

reject_array(move_reuse FLECHA-S0059 [=[
void main() {
    values := array::Array<i32>::init();
    moved := values;
    values.len();
}
]=])
reject_array(private_field FLECHA-S0044 [=[
void main() {
    values := array::Array<i32>::init();
    values.inner.clear();
}
]=])
reject_array(borrow FLECHA-S0029 [=[
void main() {
    values := array::Array<i32>::init();
    &const array::Array<i32> borrowed = &values;
    values.push(1);
    borrowed->get(0);
}
]=])
reject_array(immovable_element FLECHA-S0047 [=[
structure Immovable {
    i32 value;
};
unsafe void Immovable::tear(&self) override {}
void main() {
    values := array::Array<Immovable>::init();
    values.reserve(1);
}
]=])
