# Unsafe contexts and raw pointers

Raw pointer declarations, parameters, returns, address creation, and indirect
reads/writes are implemented. See [the runnable example](../examples/unsafe_pointers.fl).
Safe scalar and pointer-slot references now have [borrow checking](checked_references.md).

## Permission is lexical

```flecha
unsafe void write(constptr i32 destination, i32 value) {
    *destination = value;
}

i32 main() {
    i32 value = 0;
    unsafe {
        ptr := &raw value;
        write(ptr, 42);
    }
    return value;
}
```

An unsafe function may call another unsafe function directly. A safe function
must use an `unsafe { ... }` block for that call. A safe callee never inherits
permission from an unsafe caller. Nested ordinary blocks inherit their enclosing
permission; leaving an unsafe block restores the previous permission.
`unsafe write(ptr, 42);` is not valid syntax.

Ordinary and unsafe blocks create lexical scopes. Existing definite-assignment
analysis, shadowing, loop control, positions, and owning-string cleanup apply
through nested scopes. Leaving an unsafe block does not free raw pointers or
extend the lifetime of their targets.

## Raw address creation

`&raw value` produces `*T` for writable storage of type `T`, or `*const T` for
const storage. It requires unsafe permission and initialized addressable storage.
`raw` is contextual here and remains usable as an identifier elsewhere.

The operand can be a variable, a parenthesized storage location, or a pointer
dereference. Functions, literals, computed values, and cast results are not
addressable. `&raw *pointer` retrieves the designated address without loading
its pointee. Taking a raw address is not allocation.

`&value` creates a checked reference; its meaning does not change inside an
unsafe block. Reference storage cannot be exposed with `&raw ref` or `&raw *ref`. There is no implicit
reference-to-pointer conversion.

## Pointer values and storage

Copying, rebinding, passing, returning, and comparing initialized raw pointer
values do not access their targets and can occur in safe code. Both `null` and
`none` initialize a null pointer. Raw pointers are trivially zeroable, so a
pointer declared without an initializer is immediately initialized to null.

`*pointer` reads a target and `*pointer = value` writes it. Both require unsafe
permission. Assignment evaluates the target address once, then evaluates the
right-hand side, then stores the value. Parenthesized targets are supported.
Indirect stores read the pointer binding and do not prove an unrelated local
initialized through aliasing.

`==` and `!=` compare compatible pointer values and accept `null`/`none` on either
side. Strict pointer comparison (`===`, `!==`) currently requires identical
pointer types. Pointer arithmetic, ordering, explicit pointer casts, and
integer-to-pointer conversions are rejected. `*void` may carry an opaque address,
but dereferencing it is an error.

## Qualifiers

- `constptr T` / `const *T`: fixed pointer binding, writable target.
- `*const T`: rebindable pointer, read-only target.
- `constptr const T` / `const *const T`: both restrictions.
- `*T` may convert to `*const T` by adding const to the immediate target.
  Const cannot be removed. Nested target types must otherwise match exactly:
  `**T` cannot convert to `**const T`.

Unsafe permission does not bypass const checks. Each layer keeps its own
qualification: `*const *i32` cannot replace the pointed-to pointer, but may write
its final `i32` target through `**pointer`.

Indirect owning-string assignment uses the same copy and destruction rules as
direct assignment. It requires a valid, initialized owning-string target. Raw
access does not establish borrow safety or track resources across functions.

## Safety boundary and diagnostics

The programmer must ensure that each accessed pointer is non-null, properly
aligned, points to live compatible storage, and has initialized data when read.
These obligations also apply to native callers of unsafe Flecha functions.
The compiler checks lexical permission, types, qualifiers, and initialization of
pointer bindings; it does not prove arbitrary unsafe code free of null accesses,
dangling pointers, or use-after-free.

| Code | Meaning |
| --- | --- |
| `FLECHA-S0025` | Unsafe call, raw address creation, or dereference outside an unsafe context |
| `FLECHA-S0026` | Invalid raw address operand or dereference of `*void` |
| `FLECHA-S0005` | Assignment to a fixed binding or read-only target |
| `FLECHA-S0021` | Possibly uninitialized pointer or raw-address operand |
| `FLECHA-S0024` | Unsupported compound storage, such as a reference nested inside a pointer |

Allocation/free, defer, slices, volatile access, and arbitrary
pointer conversions remain outside this milestone.

## Validation

`make test` includes semantic permission/qualifier tests, CFG initialization
checks, bound storage-location tests, LLVM verification, and
`NativeSemanticTests.UnsafePointers`. The latter links Flecha functions to a C++
harness that supplies valid storage and checks scalar access, nested pointers,
null comparisons, target evaluation order, string assignment, and cleanup on
return, break, proceed, and jumpto. It exits with 42.

`make test-sanitizers` checks the compiler with ASan/UBSan and runs the native
harness with the instrumented C++ harness and runtime, including leak detection.
This does not add sanitizer instrumentation to the generated Flecha instructions.
