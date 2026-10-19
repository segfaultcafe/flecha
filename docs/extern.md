# External functions

An `extern` function is a declaration whose implementation is supplied to the
native linker. It ends with a semicolon and has no Flecha body:

```flecha
extern unsafe i32 native_add(i32 left, i32 right);
```

The external symbol name is exactly the declared identifier (`native_add` in
this example), without Flecha's module prefix. The declaration remains scoped
to its Flecha module for name resolution. External declarations must be free,
non-generic functions and cannot be lifecycle overrides or the entry point.

`extern` and `unsafe` have separate meanings. Scalar-only declarations may be
safe. A declaration containing any raw-pointer argument or result must be
`unsafe`, and callers need the ordinary unsafe permission:

```flecha
extern unsafe i32 native_read(
    *const char path,
    usize path_length,
    *u8 output,
    usize capacity
);
```

The initial C ABI supports `void` results, integer and floating-point scalars,
`bool`, `char`, and raw pointers. `bool` follows the target C `_Bool` ABI;
`char` is an unsigned 8-bit value. `isize` and `usize` match the target pointer
width. References, arrays, slices, tuples, structures, error values, and
strings or string views by value are rejected. `own` and `lend` contracts are
also rejected because external ownership summaries are not implemented.

The allocation checker rejects passing a tracked owning or lent allocation to
an extern call. Stack addresses, borrowed external pointers, and the documented
runtime string handle remain usable under `unsafe`; transferring or lending a
Flecha allocation across this boundary waits for explicit external ownership
contracts.

Raw pointers do not gain ownership, provenance, aliasing, lifetime, or
nullability guarantees at this boundary. Pointer targets may be ABI scalars,
other raw pointers, or `void`. Two explicitly documented opaque runtime handles
are also supported: mutable `*string` for a runtime-owned-string output and
`*const stringv` for a borrowed input descriptor. Ordinary `string` and
`stringv` values are never silently treated as C-compatible values. These
opaque handles are restricted to symbols with the versioned
`flecha_runtime_v1_` prefix; ordinary external libraries use byte pointers and
explicit lengths instead.

String byte storage can be exposed with `data()` only in unsafe code. It returns
`*const char`; the pointer is borrowed, may be null for an empty value, and must
not be retained beyond the source value's lifetime. Length remains explicit.
An extern parameter of type `*const u8` accepts this pointer because `char` and
`u8` have identical byte representations. This compatibility is one-way,
preserves const protection, and applies only at the extern argument boundary;
it does not introduce general raw-pointer reinterpretation.

The C or C++ definition must use C linkage and exactly matching fixed-width
types:

```cpp
extern "C" int32_t native_add(int32_t left, int32_t right) {
    return left + right;
}
```

For narrow integers, the compiler applies the target C ABI's required sign- or
zero-extension rules to both the external declaration and each call site. For
example, `i8` and `i16` use signed extension where the target ABI requires it,
while `u8`, `u16`, `char`, and `bool` use unsigned extension. Targets whose C
ABI leaves those values unextended are emitted without extension attributes.

`flecha build` accepts repeatable `--link object-or-library` arguments. Object
emission deliberately does not link. Executable builds emit Flecha code to a
unique temporary object, so an output-adjacent native input such as `program.o`
cannot be overwritten by `build source.fl -o program --link program.o`.
Longbow projects can list native objects or archives in the `[native]` manifest
section described in [longbow.md](longbow.md).
