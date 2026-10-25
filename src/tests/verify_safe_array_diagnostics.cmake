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

set(move_only_owner [=[
structure Owner {
    i32 value;
};
Owner Owner::zero() override {
    return build Owner {};
}
void Owner::tear(&self) override {}
]=])

function(reject_copy_only_array name code body)
    reject_array("${name}" "${code}" "${move_only_owner}\n${body}")
endfunction()

reject_copy_only_array(copy_only_get FLECHA-S0046 [=[
void main() {
    values := array::Array<Owner>::init();
    values.get(0);
}
]=])
reject_copy_only_array(copy_only_first FLECHA-S0046 [=[
void main() {
    values := array::Array<Owner>::init();
    values.first();
}
]=])
reject_copy_only_array(copy_only_last FLECHA-S0046 [=[
void main() {
    values := array::Array<Owner>::init();
    values.last();
}
]=])
reject_copy_only_array(copy_only_fill FLECHA-S0059 [=[
void main() {
    values := array::Array<Owner>::init();
    value := Owner::init();
    values.fill(value);
}
]=])
reject_copy_only_array(copy_only_clone FLECHA-S0046 [=[
void main() {
    values := array::Array<Owner>::init();
    values.clone();
}
]=])
reject_copy_only_array(copy_only_from_slice FLECHA-S0046 [=[
void main() {
    [1]Owner source = [Owner::init()];
    array::Array<Owner>::fromSlice(source[:]);
}
]=])
reject_copy_only_array(copy_only_append FLECHA-S0046 [=[
void main() {
    values := array::Array<Owner>::init();
    [1]Owner source = [Owner::init()];
    values.append(source[:]);
}
]=])
