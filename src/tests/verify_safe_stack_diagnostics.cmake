file(MAKE_DIRECTORY "${FLECHA_OUTPUT}")

function(reject_stack name code body)
    set(source "${FLECHA_OUTPUT}/${name}.fl")
    set(object "${FLECHA_OUTPUT}/${name}.o")
    file(WRITE "${source}" "module ${name};\n@imports :: lib::std::containers::collections;\n${body}\n")
    file(REMOVE "${object}")
    execute_process(
        COMMAND "${FLECHA_COMPILER}" --emit-object "${source}"
            --lib "std=${FLECHA_STD}" -o "${object}"
        RESULT_VARIABLE result ERROR_VARIABLE errors TIMEOUT 30)
    if(result EQUAL 0 OR EXISTS "${object}" OR NOT errors MATCHES "${code}")
        message(FATAL_ERROR "${name}: expected ${code} without object emission: ${errors}")
    endif()
endfunction()

reject_stack(move_reuse FLECHA-S0059 [=[
void main() {
    values := collections::Stack<i32>::init();
    moved := values;
    values.len();
}
]=])

reject_stack(private_inner FLECHA-S0044 [=[
void main() {
    values := collections::Stack<i32>::init();
    values.inner.clear();
}
]=])

reject_stack(active_borrow_mutation FLECHA-S0029 [=[
void main() {
    values := collections::Stack<i32>::init();
    &const collections::Stack<i32> borrowed = &values;
    values.push(1);
    borrowed->len();
}
]=])

reject_stack(immovable_element FLECHA-S0047 [=[
structure Immovable {
    i32 value;
};

unsafe void Immovable::tear(&self) override {}

void main() {
    values := collections::Stack<Immovable>::init();
    values.reserve(1);
}
]=])

reject_stack(borrowed_slice_element FLECHA-S0047 [=[
void main() {
    values := collections::Stack<[]i32>::init();
    values.reserve(1);
}
]=])

reject_stack(borrowed_reference_element FLECHA-S0047 [=[
void main() {
    values := collections::Stack<&i32>::init();
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

function(reject_move_only_stack name code body)
    reject_stack("${name}" "${code}" "${move_only_owner}\n${body}")
endfunction()

reject_move_only_stack(push_argument_reuse FLECHA-S0059 [=[
void main() {
    values := collections::Stack<Owner>::init();
    owner := Owner::init();
    values.push(owner);
    owner.value;
}
]=])

reject_move_only_stack(extracted_owner_reuse FLECHA-S0059 [=[
void main() {
    values := collections::Stack<Owner>::init();
    owner := Owner::init();
    values.push(owner);
    extracted, failure := values.pop();
    destination := collections::Stack<Owner>::init();
    destination.push(extracted);
    extracted.value;
}
]=])

reject_move_only_stack(copy_only_top FLECHA-S0046 [=[
void main() {
    values := collections::Stack<Owner>::init();
    values.top();
}
]=])
