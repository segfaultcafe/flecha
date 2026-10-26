file(MAKE_DIRECTORY "${FLECHA_OUTPUT}")

function(reject_deque name code body)
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

reject_deque(move_reuse FLECHA-S0059 [=[
void main() {
    values := collections::Deque<i32>::init();
    moved := values;
    values.len();
}
]=])

reject_deque(private_storage FLECHA-S0044 [=[
void main() {
    values := collections::Deque<i32>::init();
    values.storage.capacity();
}
]=])

reject_deque(active_borrow_mutation FLECHA-S0029 [=[
void main() {
    values := collections::Deque<i32>::init();
    &const collections::Deque<i32> borrowed = &values;
    values.pushBack(1);
    borrowed->len();
}
]=])

reject_deque(immovable_element FLECHA-S0047 [=[
structure Immovable {
    i32 value;
};

unsafe void Immovable::tear(&self) override {}

void main() {
    values := collections::Deque<Immovable>::init();
    values.reserve(1);
}
]=])

reject_deque(borrowed_slice_element FLECHA-S0047 [=[
void main() {
    values := collections::Deque<[]i32>::init();
    values.reserve(1);
}
]=])

reject_deque(borrowed_reference_element FLECHA-S0047 [=[
void main() {
    values := collections::Deque<&i32>::init();
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

function(reject_move_only_deque name code body)
    reject_deque("${name}" "${code}" "${move_only_owner}\n${body}")
endfunction()

reject_move_only_deque(push_argument_reuse FLECHA-S0059 [=[
void main() {
    values := collections::Deque<Owner>::init();
    owner := Owner::init();
    values.pushBack(owner);
    owner.value;
}
]=])

reject_move_only_deque(extracted_owner_reuse FLECHA-S0059 [=[
void main() {
    values := collections::Deque<Owner>::init();
    owner := Owner::init();
    values.pushBack(owner);
    extracted, failure := values.popFront();
    destination := collections::Deque<Owner>::init();
    destination.pushBack(extracted);
    extracted.value;
}
]=])

reject_move_only_deque(copy_only_get FLECHA-S0046 [=[
void main() {
    values := collections::Deque<Owner>::init();
    values.get(0);
}
]=])

reject_move_only_deque(copy_only_front FLECHA-S0046 [=[
void main() {
    values := collections::Deque<Owner>::init();
    values.front();
}
]=])

reject_move_only_deque(copy_only_back FLECHA-S0046 [=[
void main() {
    values := collections::Deque<Owner>::init();
    values.back();
}
]=])
