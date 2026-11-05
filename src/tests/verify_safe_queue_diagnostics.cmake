file(MAKE_DIRECTORY "${FLECHA_OUTPUT}")

function(reject_queue name code body)
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

reject_queue(move_reuse FLECHA-S0059 [=[
void main() {
    values := collections::Queue<i32>::init();
    moved := values;
    values.len();
}
]=])

reject_queue(private_inner FLECHA-S0044 [=[
void main() {
    values := collections::Queue<i32>::init();
    values.inner.clear();
}
]=])

reject_queue(active_borrow_mutation FLECHA-S0029 [=[
void main() {
    values := collections::Queue<i32>::init();
    &const collections::Queue<i32> borrowed = &values;
    values.enqueue(1);
    borrowed->len();
}
]=])

reject_queue(immovable_element FLECHA-S0047 [=[
structure Immovable {
    i32 value;
};

unsafe void Immovable::tear(&self) override {}

void main() {
    values := collections::Queue<Immovable>::init();
    values.reserve(1);
}
]=])

reject_queue(borrowed_slice_element FLECHA-S0047 [=[
void main() {
    values := collections::Queue<[]i32>::init();
    values.reserve(1);
}
]=])

reject_queue(borrowed_reference_element FLECHA-S0047 [=[
void main() {
    values := collections::Queue<&i32>::init();
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

function(reject_move_only_queue name code body)
    reject_queue("${name}" "${code}" "${move_only_owner}\n${body}")
endfunction()

reject_move_only_queue(enqueue_argument_reuse FLECHA-S0059 [=[
void main() {
    values := collections::Queue<Owner>::init();
    owner := Owner::init();
    values.enqueue(owner);
    owner.value;
}
]=])

reject_move_only_queue(extracted_owner_reuse FLECHA-S0059 [=[
void main() {
    values := collections::Queue<Owner>::init();
    owner := Owner::init();
    values.enqueue(owner);
    extracted, failure := values.dequeue();
    destination := collections::Queue<Owner>::init();
    destination.enqueue(extracted);
    extracted.value;
}
]=])

reject_move_only_queue(copy_only_front FLECHA-S0046 [=[
void main() {
    values := collections::Queue<Owner>::init();
    values.front();
}
]=])
