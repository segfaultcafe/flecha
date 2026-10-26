file(MAKE_DIRECTORY "${FLECHA_OUTPUT}")

function(reject_hash_map name code body)
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
usize integerHash(&const i32 key) {
    return (*key).cast<usize>;
}

bool integerEqual(&const i32 left, &const i32 right) {
    return *left == *right;
}
]=])

reject_hash_map(move_reuse FLECHA-S0059 "${integer_policies}
void main() {
    values := collections::HashMap<i32, i32>::init(integerHash, integerEqual);
    moved := values;
    values.len();
}")

reject_hash_map(private_keys FLECHA-S0044 "${integer_policies}
void main() {
    values := collections::HashMap<i32, i32>::init(integerHash, integerEqual);
    values.keys.capacity();
}")

reject_hash_map(private_states FLECHA-S0044 "${integer_policies}
void main() {
    values := collections::HashMap<i32, i32>::init(integerHash, integerEqual);
    values.states.len();
}")

reject_hash_map(private_hash FLECHA-S0044 "${integer_policies}
void main() {
    values := collections::HashMap<i32, i32>::init(integerHash, integerEqual);
    values.hash;
}")

reject_hash_map(active_borrow_mutation FLECHA-S0029 "${integer_policies}
void main() {
    values := collections::HashMap<i32, i32>::init(integerHash, integerEqual);
    borrowed := &values;
    values.set(1, 10);
    borrowed->len();
}")

reject_hash_map(immovable_key FLECHA-S0047 [=[
structure Immovable {
    i32 value;
};

unsafe void Immovable::tear(&self) override {}

usize hash(&const Immovable key) {
    return key->value.cast<usize>;
}

bool equal(&const Immovable left, &const Immovable right) {
    return left->value == right->value;
}

void main() {
    values := collections::HashMap<Immovable, i32>::init(hash, equal);
    values.reserve(1);
}
]=])

reject_hash_map(immovable_value FLECHA-S0047 "${integer_policies}
structure Immovable {
    i32 value;
};

unsafe void Immovable::tear(&self) override {}

void main() {
    values := collections::HashMap<i32, Immovable>::init(
        integerHash,
        integerEqual
    );
    values.reserve(1);
}")

reject_hash_map(borrowed_slice_key FLECHA-S0047 [=[
usize hash(&const []i32 key) {
    return key->len();
}

bool equal(&const []i32 left, &const []i32 right) {
    return left->len() == right->len();
}

void main() {
    values := collections::HashMap<[]i32, i32>::init(hash, equal);
    values.reserve(1);
}
]=])

reject_hash_map(borrowed_reference_value FLECHA-S0047 "${integer_policies}
void main() {
    values := collections::HashMap<i32, &i32>::init(
        integerHash,
        integerEqual
    );
    values.reserve(1);
}")

set(move_only_owner [=[
structure Owner {
    i32 value;
};

Owner Owner::zero() override {
    return build Owner {};
}

void Owner::tear(&self) override {}

usize ownerHash(&const Owner key) {
    return key->value.cast<usize>;
}

bool ownerEqual(&const Owner left, &const Owner right) {
    return left->value == right->value;
}
]=])

function(reject_move_only_hash_map name code body)
    reject_hash_map("${name}" "${code}" "${move_only_owner}\n${body}")
endfunction()

reject_move_only_hash_map(key_argument_reuse FLECHA-S0059 [=[
void main() {
    values := collections::HashMap<Owner, i32>::init(ownerHash, ownerEqual);
    key := Owner::init();
    values.set(key, 10);
    key.value;
}
]=])

reject_move_only_hash_map(value_argument_reuse FLECHA-S0059 [=[
usize integerHash(&const i32 key) {
    return (*key).cast<usize>;
}

bool integerEqual(&const i32 left, &const i32 right) {
    return *left == *right;
}

void main() {
    values := collections::HashMap<i32, Owner>::init(
        integerHash,
        integerEqual
    );
    value := Owner::init();
    values.set(1, value);
    value.value;
}
]=])

reject_move_only_hash_map(copy_only_get FLECHA-S0046 [=[
usize integerHash(&const i32 key) {
    return (*key).cast<usize>;
}

bool integerEqual(&const i32 left, &const i32 right) {
    return *left == *right;
}

void main() {
    values := collections::HashMap<i32, Owner>::init(
        integerHash,
        integerEqual
    );
    i32 key = 1;
    values.get(&key);
}
]=])
