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
reject_array(shared_view_push FLECHA-S0029 [=[
void main() {
    values := array::Array<i32>::init();
    values.push(1);
    view := values.view();
    values.push(2);
    view[0];
}
]=])
reject_array(shared_view_set FLECHA-S0029 [=[
void main() {
    values := array::Array<i32>::init();
    values.push(1);
    view := values.view();
    values.set(0, 2);
    view[0];
}
]=])
reject_array(shared_view_reserve FLECHA-S0029 [=[
void main() {
    values := array::Array<i32>::init();
    view := values.view();
    values.reserve(0);
    view.len();
}
]=])
reject_array(shared_view_clear FLECHA-S0029 [=[
void main() {
    values := array::Array<i32>::init();
    view := values.view();
    values.clear();
    view.len();
}
]=])
reject_array(shared_view_pop FLECHA-S0029 [=[
void main() {
    values := array::Array<i32>::init();
    values.push(1);
    view := values.view();
    values.pop();
    view[0];
}
]=])
reject_array(shared_view_remove FLECHA-S0029 [=[
void main() {
    values := array::Array<i32>::init();
    values.push(1);
    view := values.view();
    values.remove(0);
    view[0];
}
]=])
reject_array(shared_view_swap_remove FLECHA-S0029 [=[
void main() {
    values := array::Array<i32>::init();
    values.push(1);
    view := values.view();
    values.swapRemove(0);
    view[0];
}
]=])
reject_array(shared_view_exclusive_view FLECHA-S0029 [=[
void main() {
    values := array::Array<i32>::init();
    values.push(1);
    shared := values.view();
    exclusive := values.viewRef();
    shared[0];
    exclusive[0] = 2;
}
]=])
reject_array(shared_view_move FLECHA-S0029 [=[
void consume(array::Array<i32> values) {}
void main() {
    values := array::Array<i32>::init();
    values.push(1);
    view := values.view();
    consume(values);
    view[0];
}
]=])
reject_array(shared_view_replace FLECHA-S0029 [=[
void main() {
    values := array::Array<i32>::init();
    values.push(1);
    view := values.view();
    values = array::Array<i32>::init();
    view[0];
}
]=])
reject_array(exclusive_view_shared_view FLECHA-S0029 [=[
void main() {
    values := array::Array<i32>::init();
    values.push(1);
    exclusive := values.viewRef();
    shared := values.view();
    exclusive[0] = 2;
    shared[0];
}
]=])
reject_array(exclusive_view_exclusive_view FLECHA-S0029 [=[
void main() {
    values := array::Array<i32>::init();
    values.push(1);
    first := values.viewRef();
    second := values.viewRef();
    first[0] = 2;
    second[0] = 3;
}
]=])
reject_array(exclusive_view_owner_access FLECHA-S0029 [=[
void main() {
    values := array::Array<i32>::init();
    values.push(1);
    exclusive := values.viewRef();
    values.len();
    exclusive[0] = 2;
}
]=])
reject_array(nested_inner_view_outer_growth FLECHA-S0029 [=[
void main() {
    outer := array::Array<array::Array<i32>>::init();
    inner := array::Array<i32>::init();
    inner.push(1);
    outer.push(inner);
    innerView := outer.viewRef()[0].view();
    replacement := array::Array<i32>::init();
    outer.push(replacement);
    innerView[0];
}
]=])
reject_array(shared_owning_slice_view_mutation FLECHA-S0005 [=[
void main() {
    values := array::Array<own []i32>::init();
    owner, failure := new<own []i32>(1);
    values.push(owner);
    view := values.view();
    view[0][0] = 1;
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
reject_array(borrowed_slice_element FLECHA-S0047 [=[
void main() {
    values := array::Array<[]i32>::init();
    values.reserve(1);
}
]=])
reject_array(borrowed_reference_element FLECHA-S0047 [=[
void main() {
    values := array::Array<&i32>::init();
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

reject_copy_only_array(partial_indexed_move FLECHA-S0046 [=[
void main() {
    values := array::Array<Owner>::init();
    owner := Owner::init();
    values.push(owner);
    view := values.viewRef();
    extracted := view[0];
}
]=])
reject_copy_only_array(push_argument_reuse FLECHA-S0059 [=[
void main() {
    values := array::Array<Owner>::init();
    owner := Owner::init();
    values.push(owner);
    owner.value;
}
]=])
reject_copy_only_array(extracted_owner_reuse FLECHA-S0059 [=[
void main() {
    values := array::Array<Owner>::init();
    owner := Owner::init();
    values.push(owner);
    extracted, failure := values.pop();
    destination := array::Array<Owner>::init();
    destination.push(extracted);
    extracted.value;
}
]=])

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
