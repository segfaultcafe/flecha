if(NOT DEFINED FLECHA_COMPILER OR NOT DEFINED FLECHA_STD OR
   NOT DEFINED FLECHA_OUTPUT)
    message(FATAL_ERROR "slice diagnostic verification requires compiler, std, and output paths")
endif()

file(MAKE_DIRECTORY "${FLECHA_OUTPUT}")

function(reject_slice name diagnostic tear_declaration body)
    set(source "${FLECHA_OUTPUT}/${name}.fl")
    set(object "${FLECHA_OUTPUT}/${name}.o")
    file(WRITE "${source}"
        "module ${name};\n"
        "@imports :: lib::std::memory::slices;\n"
        "structure Element { i32 value; };\n"
        "${tear_declaration}\n"
        "Element Element::zero() override { return build Element {}; }\n"
        "${body}\n")
    file(REMOVE "${object}")

    execute_process(
        COMMAND "${FLECHA_COMPILER}" --emit-object "${source}"
            --lib "std=${FLECHA_STD}" -o "${object}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        TIMEOUT 30)

    if(result EQUAL 0 OR EXISTS "${object}" OR
       NOT errors MATCHES "${diagnostic}:")
        message(FATAL_ERROR
            "${name}: expected ${diagnostic} without object emission:\n"
            "${output}${errors}")
    endif()
endfunction()

set(move_only_tear "void Element::tear(&self) override {}")
set(immovable_tear "unsafe void Element::tear(&self) override {}")

set(copy_elements_body [=[
void reject([]Element destination, []const Element source) {
    slices::copy_elements<Element>(destination, source);
}
]=])

set(copy_within_body [=[
void reject([]Element values) {
    slices::copy_within<Element>(values, 0, 0, 0);
}
]=])

set(fill_body [=[
void reject([]Element values, Element value) {
    slices::fill<Element>(values, value);
}
]=])

set(swap_body [=[
void reject([]Element values) {
    slices::swap<Element>(values, 0, 0);
}
]=])

reject_slice(
    move_only_copy_elements FLECHA-S0046 "${move_only_tear}"
    "${copy_elements_body}")
reject_slice(
    move_only_copy_within FLECHA-S0046 "${move_only_tear}"
    "${copy_within_body}")
reject_slice(
    move_only_fill FLECHA-S0059 "${move_only_tear}"
    "${fill_body}")
reject_slice(
    move_only_swap FLECHA-S0046 "${move_only_tear}"
    "${swap_body}")

reject_slice(
    immovable_copy_elements FLECHA-S0046 "${immovable_tear}"
    "${copy_elements_body}")
reject_slice(
    immovable_copy_within FLECHA-S0046 "${immovable_tear}"
    "${copy_within_body}")
reject_slice(
    immovable_fill FLECHA-S0046 "${immovable_tear}"
    "${fill_body}")
reject_slice(
    immovable_swap FLECHA-S0046 "${immovable_tear}"
    "${swap_body}")
