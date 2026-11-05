file(MAKE_DIRECTORY "${FLECHA_OUTPUT}")

function(reject_hash_set name code body)
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

set(integer_policies [=[
usize integerHash(&const i32 value) {
    return (*value).cast<usize>;
}

bool integerEqual(&const i32 left, &const i32 right) {
    return *left == *right;
}
]=])

reject_hash_set(move_reuse FLECHA-S0059 "${integer_policies}
void main() {
    values := collections::HashSet<i32>::init()
        .withHash(integerHash).withEqual(integerEqual);
    moved := values;
    values.len();
}")

reject_hash_set(private_inner FLECHA-S0044 "${integer_policies}
void main() {
    values := collections::HashSet<i32>::init()
        .withHash(integerHash).withEqual(integerEqual);
    values.inner.capacity();
}")

reject_hash_set(active_borrow_mutation FLECHA-S0029 "${integer_policies}
void main() {
    values := collections::HashSet<i32>::init()
        .withHash(integerHash).withEqual(integerEqual);
    borrowed := &values;
    values.insert(1);
    borrowed->len();
}")

reject_hash_set(immovable_element FLECHA-S0047 [=[
structure Immovable {
    i32 value;
};

unsafe void Immovable::tear(&self) override {}

usize hash(&const Immovable value) {
    return value->value.cast<usize>;
}

bool equal(&const Immovable left, &const Immovable right) {
    return left->value == right->value;
}

void main() {
    values := collections::HashSet<Immovable>::init()
        .withHash(hash).withEqual(equal);
    values.reserve(1);
}
]=])

reject_hash_set(borrowed_slice_element FLECHA-S0047 [=[
usize hash(&const []i32 value) {
    return value->len();
}

bool equal(&const []i32 left, &const []i32 right) {
    return left->len() == right->len();
}

void main() {
    values := collections::HashSet<[]i32>::init()
        .withHash(hash).withEqual(equal);
    values.reserve(1);
}
]=])

reject_hash_set(borrowed_reference_element FLECHA-S0047 [=[
usize hash(&const &i32 value) {
    return (**value).cast<usize>;
}

bool equal(&const &i32 left, &const &i32 right) {
    return **left == **right;
}

void main() {
    values := collections::HashSet<&i32>::init()
        .withHash(hash).withEqual(equal);
    values.reserve(1);
}
]=])

reject_hash_set(insert_argument_reuse FLECHA-S0059 [=[
structure Owner {
    i32 value;
};

Owner Owner::zero() override {
    return build Owner {};
}

void Owner::tear(&self) override {}

usize hash(&const Owner value) {
    return value->value.cast<usize>;
}

bool equal(&const Owner left, &const Owner right) {
    return left->value == right->value;
}

void main() {
    values := collections::HashSet<Owner>::init()
        .withHash(hash).withEqual(equal);
    owner := Owner::init();
    values.insert(owner);
    owner.value;
}
]=])
