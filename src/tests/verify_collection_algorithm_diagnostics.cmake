if(NOT DEFINED FLECHA_COMPILER OR NOT DEFINED FLECHA_STD OR
   NOT DEFINED FLECHA_OUTPUT)
    message(FATAL_ERROR "collection algorithm diagnostics require compiler, std, and output paths")
endif()

file(MAKE_DIRECTORY "${FLECHA_OUTPUT}")

function(reject_algorithm name diagnostic tear_declaration body)
    set(source "${FLECHA_OUTPUT}/${name}.fl")
    set(object "${FLECHA_OUTPUT}/${name}.o")
    file(WRITE "${source}"
        "module ${name};\n"
        "@imports :: lib::std::containers::collections;\n"
        "structure Element { i32 value; };\n"
        "${tear_declaration}\n"
        "Element Element::zero() override { return build Element {}; }\n"
        "bool less(&const Element left, &const Element right) { return left->value < right->value; }\n"
        "bool equal(&const Element left, &const Element right) { return left->value == right->value; }\n"
        "bool accepted(&const Element value) { return value->value > 0; }\n"
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

set(reverse_body [=[
void reject([]Element values) {
    collections::reverse<Element>(values);
}
]=])

set(sort_body [=[
void reject([]Element values) {
    collections::sort_by<Element>(values, less);
}
]=])

set(partition_body [=[
void reject([]Element values) {
    collections::partition_by<Element>(values, accepted);
}
]=])

set(dedup_body [=[
void reject([]Element values) {
    collections::dedup_by<Element>(values, equal);
}
]=])

foreach(category IN ITEMS move_only immovable)
    if(category STREQUAL "move_only")
        set(tear_declaration "${move_only_tear}")
    else()
        set(tear_declaration "${immovable_tear}")
    endif()

    reject_algorithm(
        ${category}_reverse FLECHA-S0046 "${tear_declaration}"
        "${reverse_body}")
    reject_algorithm(
        ${category}_sort FLECHA-S0046 "${tear_declaration}"
        "${sort_body}")
    reject_algorithm(
        ${category}_partition FLECHA-S0046 "${tear_declaration}"
        "${partition_body}")
    reject_algorithm(
        ${category}_dedup FLECHA-S0046 "${tear_declaration}"
        "${dedup_body}")
endforeach()
