file(MAKE_DIRECTORY "${FLECHA_OUTPUT}")

function(reject_binary_heap name code body)
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

set(integer_less [=[
bool less(&const i32 left, &const i32 right) {
    return *left < *right;
}
]=])

reject_binary_heap(move_reuse FLECHA-S0059 "${integer_less}
void main() {
    values := collections::BinaryHeap<i32>::init(less);
    moved := values;
    values.len();
}")

reject_binary_heap(private_storage FLECHA-S0044 "${integer_less}
void main() {
    values := collections::BinaryHeap<i32>::init(less);
    values.storage.capacity();
}")

reject_binary_heap(private_length FLECHA-S0044 "${integer_less}
void main() {
    values := collections::BinaryHeap<i32>::init(less);
    values.length;
}")

reject_binary_heap(private_comparator FLECHA-S0044 "${integer_less}
void main() {
    values := collections::BinaryHeap<i32>::init(less);
    values.less;
}")

reject_binary_heap(active_borrow_mutation FLECHA-S0029 "${integer_less}
void main() {
    values := collections::BinaryHeap<i32>::init(less);
    &const collections::BinaryHeap<i32> borrowed = &values;
    values.push(1);
    borrowed->len();
}")

reject_binary_heap(immovable_element FLECHA-S0047 [=[
structure Immovable {
    i32 value;
};

unsafe void Immovable::tear(&self) override {}

bool less(&const Immovable left, &const Immovable right) {
    return left->value < right->value;
}

void main() {
    values := collections::BinaryHeap<Immovable>::init(less);
    values.reserve(1);
}
]=])

reject_binary_heap(borrowed_slice_element FLECHA-S0047 [=[
bool less(&const []i32 left, &const []i32 right) {
    return left->len() < right->len();
}

void main() {
    values := collections::BinaryHeap<[]i32>::init(less);
    values.reserve(1);
}
]=])

reject_binary_heap(borrowed_reference_element FLECHA-S0047 [=[
bool less(&const &i32 left, &const &i32 right) {
    return **left < **right;
}

void main() {
    values := collections::BinaryHeap<&i32>::init(less);
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

bool less(&const Owner left, &const Owner right) {
    return left->value < right->value;
}
]=])

function(reject_move_only_binary_heap name code body)
    reject_binary_heap("${name}" "${code}" "${move_only_owner}\n${body}")
endfunction()

reject_move_only_binary_heap(push_argument_reuse FLECHA-S0059 [=[
void main() {
    values := collections::BinaryHeap<Owner>::init(less);
    owner := Owner::init();
    values.push(owner);
    owner.value;
}
]=])

reject_move_only_binary_heap(extracted_owner_reuse FLECHA-S0059 [=[
void main() {
    values := collections::BinaryHeap<Owner>::init(less);
    owner := Owner::init();
    values.push(owner);
    extracted, failure := values.pop();
    destination := collections::BinaryHeap<Owner>::init(less);
    destination.push(extracted);
    extracted.value;
}
]=])

reject_move_only_binary_heap(copy_only_top FLECHA-S0046 [=[
void main() {
    values := collections::BinaryHeap<Owner>::init(less);
    values.top();
}
]=])
