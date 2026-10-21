# Flecha Language Specification

Status: evolving specification; implemented and proposed behavior is identified
File extension: `.fl`  
Compiler command: `flecha`

## 1. Purpose

Flecha is a low-level, statically typed programming language for software that needs predictable execution, explicit control over memory, and direct access to hardware and operating-system facilities.

The language is intended for systems software such as kernels, embedded programs, runtimes, game engines, databases, command-line tools, and performance-sensitive libraries. It should provide C-like control while reducing accidental undefined behavior and improving type safety, diagnostics, and tooling.

Flecha should make the cost and behavior of code locally visible. Allocation, unsafe memory access, control transfer, and conversions must not be hidden behind surprising language behavior.

## 2. Design goals

Flecha aims to provide:

- deterministic execution without a garbage collector;
- predictable data representation and calling conventions;
- zero-cost abstractions where practical;
- explicit unsafe boundaries;
- strong static typing without a large or academic-looking syntax;
- first-class tuples and multiple return values;
- a zero value for every primitive type;
- direct comparison with the universal `none` literal;
- simple interoperability with C;
- fast compilation and useful diagnostics;
- a small language core that can eventually compile itself.

Flecha does not initially aim to provide:

- classes or implementation inheritance;
- exceptions;
- an implicit garbage-collected runtime;
- hidden heap allocation;
- a sophisticated borrow checker in the first implementation;
- generators, coroutines, or asynchronous functions in the first implementation;
- a stable binary interface between arbitrary Flecha compiler versions.

## 3. Source files and entry points

Flecha source files use the `.fl` extension.

```text
main.fl
```

A hosted executable may use an integer-returning entry point:

```flecha
i32 main() {
    return 0;
}
```

A freestanding environment may provide its own entry point and permit `void`:

```flecha
unsafe void main() {
    return;
}
```

The precise hosted and freestanding entry-point rules remain to be specified.

## 4. Lexical conventions

Identifiers begin with a letter or underscore and may subsequently contain letters, digits, or underscores. Flecha is case-sensitive.

```flecha
value
source_length
CopyError
```

Line and block comments use familiar C-style syntax:

```flecha
// A line comment

/* A block comment */
```

Statements are terminated with semicolons. Blocks are enclosed in braces.

## 5. Functions

Flecha function declarations place modifiers first, followed by the return type, function name, parameters, and body:

```text
[modifiers] ReturnType functionName(parameters) [<- borrowSource] {
    body
}
```

Examples:

```flecha
void reset() {
    return;
}

i32 add(i32 left, i32 right) {
    return left + right;
}

unsafe void write_value(lend *u32 address, u32 value) {
    *address = value;
    return;
}

&const i32 view(&const i32 value) <- value {
    return value;
}
```

Tentative function modifiers include:

- `unsafe`
- `export`
- `extern`
- `inline`
- `comptime`

Modifiers and their exact interactions will be specified as their corresponding features are implemented.

Flecha 0.2.0 introduces named callable signatures:

```flecha
func Mapper(i32) => (i32);
func Logger(stringv) => ();
unsafe func Reader(lend *const u8 source) => (i32);
once func Task() => (i32);
```

A named signature is a normal type. Parameter names are optional unless a
borrowed result names its source. Results always use a list: `()` means `void`,
one entry is a single result, and multiple entries form a tuple result.
Ownership contracts, `unsafe`, and `once` are part of structural
compatibility.

Named functions can be stored, passed, returned, inferred by `:=`, and called
indirectly:

```flecha
func Mapper(i32) => (i32);

i32 twice(i32 value) { return value * 2; }
i32 apply(Mapper callback, i32 value) { return callback(value); }

i32 main() {
    callback := twice;
    return apply(callback, 21);
}
```

Callable signatures may be generic. Type arguments are explicit when the alias
is used, and specialization substitutes parameters and results before ordinary
structural compatibility, safety, ownership, and borrow-contract checking:

```flecha
func Predicate<T>(&const T value) => (bool);
func Transform<T, U>(T value) => (U);

Transform<i32, i64> widen = (value) => {
    return value.cast<i64>;
};
```

A specialized alias is an ordinary callable type. It can be used in parameters,
results, fields, containers, and other generic declarations, and it supplies the
complete expected signature for contextual anonymous-function inference.
Generic callable declarations do not infer omitted type arguments.

Callable aliases are structural rather than nominal: two complete signatures
with identical parameter, result, safety, ownership, and borrow contracts are
compatible regardless of their declaration names. Callable values are
move-only and use a uniform compiler ABI containing invoke, environment, and
destroy pointers. Captureless named functions use null environment and destroy
pointers.

`once func` is a distinct callable kind whose invocation consumes the callable
value. A second invocation is diagnosed as a moved-value use. A compatible
reusable callable may move into a once slot, but a once callable cannot become
reusable. Both kinds use the same compiler ABI.

Captureless anonymous functions use the same signature shape. The empty square
brackets may be written explicitly or omitted:

```flecha
Mapper explicit = [](i32 value) => (i32) { return value * 2; };
Mapper inferred = (value) => { return value * 2; };
```

An explicit literal spells every parameter type and its complete result list.
A contextually inferred literal uses bare parameter names and omits the result
list; its entire signature comes from the expected callable type. Inference is
available in typed initializers, arguments, and returns, but not in `:=`
declarations without an expected type. Partial inference is rejected.

An omitted or empty capture list is captureless: names in the body resolve from
module scope, not the surrounding local scope. Local values must be listed
explicitly. Snapshot and checked-reference captures are supported:

```flecha
i32 factor = 2;
Mapper scaled = [copy factor](value) => {
    return value * factor;
};

i32 count = 0;
Mapper incremented = [&count, &const factor](value) => {
    count = count + 1;
    return value * factor + count;
};
```

Each `copy` entry snapshots its source when the anonymous function value is
created. Scalars and copyable aggregates are copied by value; owning strings
and owning fields are deep-copied. Reassigning or leaving the source scope does
not affect the captured value. Captured state belongs to the closure and may be
mutated independently when its source binding was writable.

Copy captures reject borrowed handles, callable values, and other noncopyable
values. A raw pointer may be copied only by an `unsafe` anonymous function.
Tracked raw allocations require an explicit resource capture instead:

```flecha
unsafe Reader inspect(lend *const i32 pointer) {
    return unsafe [lend pointer]() => { return *pointer; }; // rejected: escape
}

unsafe Reader retain(own *i32 pointer) {
    return unsafe [own pointer]() => { return *pointer; };
}
```

`lend name` keeps a tracked raw allocation with its current owner and prevents
freeing or transferring that allocation while the closure is live. A lending
closure cannot leave the lexical scope in which it was created. It requires an
`unsafe` anonymous function and a raw pointer or runtime-sized buffer.

`own name` moves a value into the closure environment. For a raw pointer or
runtime-sized buffer, it transfers cleanup responsibility and requires an
`unsafe` anonymous function. It also accepts movable, noncopyable safe values,
including callable values and aggregates containing callables or safe custom
teardown. These captures do not make the anonymous function unsafe. Copyable
values use `copy` instead, and values with unsafe teardown are immovable and
rejected. The source binding becomes unavailable, borrow origins follow the
moved value, and the environment destroy thunk recursively tears it down
exactly once.

All `own` captured bindings are read-only. Because ordinary callables are
reusable, their bodies may access an owned capture but cannot transfer or
otherwise consume it. A `once` anonymous function may move, return, pass to an
`own` parameter, or free an owned capture because its body can execute only
once. Consumed captures are removed from the environment's cleanup obligation;
untouched captures are still destroyed when the callable is destroyed without
being invoked.

`&name` creates an exclusive checked borrow of writable storage. `&const name`
creates a shared, read-only borrow. The loan lasts until the closure's last use,
including when the closure moves into another variable or a callable-containing
array, tuple, or structure. A closure cannot be stored somewhere that outlives
the captured storage. Returning a borrowing closure requires a matching
`<- parameter` contract, and calls propagate that origin into the returned
callable value. Borrowed reference, slice, and string-view parameters are
stored in the environment by value so returned closures retain the underlying
borrow rather than the creator's parameter slot.

Callable values remain move-only, and moving a closure transfers its
environment and cleanup responsibility exactly once. Anonymous functions use
the `once` modifier to opt into a consuming body:

```flecha
once func Task() => (i32);

Task task = once [own resource]() => (i32) {
    return consume(resource);
};
```

## 6. Return and yield

`return` ends a function and optionally supplies its final result:

```flecha
i32 identity(i32 value) {
    return value;
}
```

```flecha
void stop() {
    return;
}
```

`yield` is reserved for a future suspension construct. It will not be an alternative spelling of `return`.

The intended distinction is:

- `return` permanently ends the current function invocation;
- `yield` produces an intermediate value and suspends a generator or coroutine so it can later resume.

Generators and coroutines are outside the initial language core. Reserving `yield` now prevents incompatible syntax later.

## 7. Primitive types

The initial primitive type set is expected to include:

| Category | Types |
| --- | --- |
| Signed integers | `i8`, `i16`, `i32`, `i64`, `isize` |
| Unsigned integers | `u8`, `u16`, `u32`, `u64`, `usize` |
| Floating point | `f32`, `f64` |
| Logical | `bool` |
| Character | `char` |
| No value | `void` |
| Pointer | `*T`, with applicable qualifiers |

Integer sizes are part of their type names. `isize` and `usize` have the native pointer width of the compilation target.

Implicit conversion between distinct primitive types should be avoided. In particular, integers, booleans, and pointers remain differently typed even when their representations or zero values coincide.

### 7.1 Enums

Enums are nominal scalar types with an integer backing and qualified variants:

```flecha
enum State: u8 {
    Off,
    default Unknown = 7,
    On,
}
```

The backing defaults to `u32`. Explicit and implicit discriminants must fit the
backing type and remain unique. At most one variant may be marked `default`;
that variant is the contextual value of `none`. An enum without a default does
not accept `none`. Enums are copyable, compare only with the same nominal enum,
and may be stored in aggregates and accessed through references or pointers.

Enums support out-of-line instance and static member functions using the same
receiver spelling, privacy, and call syntax as structure methods. `init`,
`zero`, and `tear` remain compiler-reserved because enum lifecycle is intrinsic.
See [enums](docs/enums.md) for implemented syntax and restrictions.

### 7.2 Enum matching

`match` is an expression over a nominal enum value. Arms use `=>`; patterns are
qualified enum variants or `_`:

```flecha
i32 status_code(State state) {
    return match state {
        State.Off => 0,
        State.Unknown => 1,
        State.On => 2,
    };
}
```

Every variant must appear exactly once unless a final wildcard arm covers the
remaining variants. Patterns from another enum, unknown variants, duplicates,
and arms following `_` are errors. Every arm is an expression and must produce
a compatible result type. The matched value is evaluated once and only the
selected arm is evaluated. See [enum matching](docs/match.md).

## 8. Universal zero and `none`

Every primitive Flecha type has a defined zero value. `none` is a universal, polymorphic zero literal that can be converted to the zero value of a destination or comparison type.

Tentative zero values are:

| Type category | Zero value |
| --- | --- |
| Integer | `0` |
| Floating point | positive zero, `+0.0` |
| `bool` | `false` |
| `char` | the null character, `'\0'` |
| Pointer | `null` |
| Error type | its designated no-error value |
| Enum with a default variant | its designated default variant |
| Plain structure | recursively defaulted fields |

Examples:

```flecha
i32 count = none;       // i32(0)
bool ready = none;      // false
*u8 address = none;     // null pointer
Position position = none; // Position::zero()
```

After conversion, a value does not retain metadata recording that it originated from the `none` literal:

```flecha
i32 count = none;

count == none;      // true
count === none;     // false
count === i32(0);   // true
```

For primitives, `none` means the zero value rather than a separately representable absence state. Consequently, a primitive integer cannot distinguish “no integer” from integer zero. A future tagged optional type may represent that distinction when required.

Plain structures receive recursively generated default construction when all
fields support it. A structure with custom initialization or destruction must
provide `T T::zero() override`. The override is static, parameterless, safe,
side-effect-free, non-allocating, and non-recursive. Custom defaults produce a
once-per-structure compilation warning. Fixed arrays default each element.

### 8.1 Initialization

Ordinary typed declarations may omit an initializer, but the variable must be
assigned on every reachable path before it is read:

```flecha
i32 count;          // not initialized
count = 42;         // now readable
bool ready = none;  // explicitly initialized to false
```

The compiler checks definite assignment through branches, loops, and jumps.
Owning `string` supports delayed initialization; borrowed `stringv` requires
an initializer. See [definite assignment](docs/definite_assignment.md).

### 8.2 Const declarations

`const T name = value;` initializes a fixed binding. Its initializer may be a
runtime expression; `const` does not imply compile-time evaluation. Assignment
to the binding is an error, even if the assigned value is unchanged. A const
local must have an initializer; delayed const initialization is not supported.

```flecha
i32 add(const i32 input) {
    const i32 increment = 2;
    return input + increment;
}
```

Const parameters are initialized by the caller and cannot be reassigned in the
callee. Copying a const value does not make the destination const. A top-level
const qualifier on a return or cast type likewise does not qualify the receiving
binding. Nested target qualifiers remain part of type identity. Existing owning
string copies and borrowed-view lifetime checks remain in force.

The implemented declaration form uses an explicit type; `name := value` still
creates a mutable binding. `constval` is an ordinary identifier, not a keyword.
See [const declarations and compound types](docs/const_types.md).

## 9. Equality

Flecha distinguishes value equality from strict type-and-value equality.

### 9.1 Value equality

`==` and `!=` compare values. When exactly one operand is the literal `none`, `none` is contextually converted to the zero value of the other operand's type.

```flecha
0 == none;       // true
false == none;   // true
null == none;    // true
```

This special treatment does not generally make differently typed concrete values comparable:

```flecha
i32 signed_zero = 0;
u32 unsigned_zero = 0;

signed_zero == unsigned_zero; // compile-time type error
0 == false;                   // compile-time type error
```

The initial equality rule is:

1. If both operands have the same comparable type, compare their values.
2. If exactly one operand has type `none`, convert it to the other operand's zero value and compare.
3. Otherwise, report a type error.

Structure value equality is field-by-field for comparable fields. Operands are
evaluated once without copying or consuming their source values. A structure
compared strictly with `none` is unequal without invoking default construction.

### 9.2 Strict equality

`===` and `!==` compare without contextual conversion. Strict equality succeeds only when both operands have the same type and equal values.

```flecha
0 === none;       // false
false === none;   // false
none === none;    // true
i32(0) === i32(0); // true
i32(0) === u32(0); // false
```

Strict equality is semantic type-and-value equality, not necessarily raw byte equality. Padding, floating-point representations, and other representation details must not silently turn `===` into a memory comparison. A separate explicit operation may be provided for comparing raw representations.

The behavior of floating-point NaNs and signed zero under both equality operators remains to be specified.

## 10. Tuples and multiple return values

Tuples are first-class values. A tuple type is written as a comma-separated type list inside parentheses:

```flecha
(i32, bool)
(usize, CopyError)
```

Functions can return tuples directly:

```flecha
(i32, i32) divide(i32 dividend, i32 divisor) {
    return (dividend / divisor, dividend % divisor);
}
```

Tuple destructuring binds the returned elements:

```flecha
quotient, remainder := divide(10, 3);
```

Explicitly typed destructuring is also supported:

```flecha
i32 quotient, i32 remainder = divide(10, 3);
i32 q, r = divide(10, 3); // One type applies to both names.
```

Tuples, tuple parameters/results, and flat destructuring are implemented.
Binding-list parentheses are optional. Without parentheses, explicit types
carry forward to subsequent names until another type is written. Mixed
bindings such as `(existing, i32 fresh) = value;` and `_` discards are supported.
In a `:=` binding list, names already declared in the current lexical scope are
assigned while new names are declared; at least one non-blank name must be new.
The right-hand side evaluates once before destination stores. Tuples use value
semantics, including deep copies of owning strings; stored borrowed elements
remain unsupported. The native representation is an LLVM non-packed struct,
with a compiler-internal aggregate calling convention. See
[tuples and destructuring](docs/tuples.md) for syntax, cleanup, and current limits.
Named and generic error values are implemented; see [error_values.md](docs/error_values.md).

## 11. Errors as values

Flecha does not use exceptions in its core error model. Errors are ordinary typed values and may be returned as tuple elements.

```flecha
error CopyError {
    InvalidPointer,
    DestinationTooSmall,
}
```

The no-error state is the error type's zero value and therefore compares equal to `none`:

```flecha
unsafe (usize, CopyError) copy(
    []u8 destination,
    []const u8 source
) {
    if destination.len < source.len {
        return (0, CopyError.DestinationTooSmall);
    }

    memory_copy(destination.ptr, source.ptr, source.len);
    return (source.len, none);
}
```

Callers inspect the error explicitly:

```flecha
unsafe {
    (copied, err) := copy(destination, source);
    if err != none {
        return (0, err);
    }
}
```

Error declarations contain one or more distinct failure names, with an optional
trailing comma. `none` is implicit. Named types only accept their own family or
`none`; built-in `error` accepts any family and preserves its identity. Conversion
from a named error to `error` is implicit, including within tuples. The reverse
conversion and numeric casts are rejected.

`==`/`!=` compare matching named families, or widen a named error when the other
operand is generic `error`. Direct comparison of two different named families is
rejected. `none` is contextually success; all named successes widen to the same
generic success. Strict `===`/`!==` perform no widening or contextual conversion:
different static error types, and error versus literal `none`, are unequal.

Error values support ordinary const, definite-assignment, aggregate, and
borrowing rules. An uninitialized error variable cannot be read. They are
allocation-free and copyable, with no payloads or automatic propagation.
See [error_values.md](docs/error_values.md) for implemented behavior and the
compiler-private representation. The allocator example above still depends on
future allocation APIs.

## 12. Unsafe code

Unsafe operations include behavior the compiler cannot fully validate. The
implemented model covers raw-address creation, raw-pointer dereferences, and
calls with caller-maintained safety requirements. Volatile memory access and
unchecked representation construction remain future features.

An `unsafe` function places safety obligations on its caller:

```flecha
unsafe void write_value(lend *u32 address, u32 value) {
    *address = value;
    return;
}

unsafe void destroy_value(own *u32 address) {
    free(address);
    return;
}

unsafe (own *u32, error) create_value(u32 initial) {
    return new<u32>(initial);
}
```

Calling it requires an explicit unsafe context:

```flecha
u32 create_and_read() {
    unsafe {
        address, allocation_error := create_value(0);
        if allocation_error != none {
            return 0;
        }
        write_value(address, 1);
        result := *address;
        destroy_value(address);
        return result;
    }
}
```

An ordinary safe function may internally use a small unsafe block while preserving a safe external contract:

```flecha
u32 increment(u32 value) {
    unsafe {
        pointer := &raw value;
        write_value(pointer, value + 1);
    }
    return value;
}
```

A function should not be marked `unsafe` merely because its implementation contains an unsafe operation. It should be marked `unsafe` when correct use depends on an invariant the compiler cannot enforce at the call site.

Unsafe functions and `unsafe { ... }` blocks are implemented. An unsafe
function can call another unsafe function directly. Safe callees do not inherit
permission from unsafe callers. The shorthand `unsafe copy(...)` is rejected;
use a block instead. Ordinary nested blocks also create lexical scopes.

Raw address creation is explicit: `&raw value` produces a raw pointer and
requires an unsafe context. `&value` creates a checked reference. Volatile
memory access and integer-to-pointer casts remain future features. The
`volatile` word is reserved by the tokenizer, but types such as
`*volatile u32` are not accepted by the parser yet.
See [unsafe contexts and pointers](docs/unsafe_pointers.md).

## 13. Pointers, arrays, and slices

The implemented pointer syntax distinguishes target constness from a fixed
pointer binding:

```flecha
*u8
*const u8
const *u8
const *const u8
```

The parser and type model represent recursive pointers and references with
const qualifiers at each layer:

| Form | Binding | Target |
| --- | --- | --- |
| `*u8` / `&u8` | Rebindable | Writable |
| `*const u8` / `&const u8` | Rebindable | Read-only |
| `const *u8` / `const &u8` | Fixed | Writable |
| `const *const u8` / `const &const u8` | Fixed | Read-only |

`constptr T` is an alias for `const *T`; `constref T` is an alias for
`const &T`. They normalize to the same types, including when nested. For example,
`&constptr u8` is `&const *u8`. Duplicate const qualifiers on one layer are rejected.
Consecutive reference markers need whitespace (`& &u8`) because `&&` is a logical
operator.

Raw pointer declarations, parameters, returns, null initialization, and equality
are implemented. `&raw value`, raw dereferences, and raw indirect assignments require
an unsafe function or block. Unsafe permission does not override constness.
Pointer values can be copied in safe code. Raw pointers carry no checked lifetime
or non-null guarantee. A pointer may add const to its immediate target, but
cannot remove const or change qualifiers deeper inside a nested target type.

Checked `&T` and `&const T` references to initialized scalar and raw-pointer
storage are implemented. `&value`, `*ref`, and `*ref = value` are safe operations.
Reference assignment rebinds; mutable handle assignment moves, shared handle
assignment copies, and reference arguments reborrow for the call. Mutable
borrows exclude conflicting access; shared borrows exclude mutation. The
compiler tracks last possible uses, borrow origins, and moved handles through
control flow. `constref T` fixes the handle without making its target read-only;
moving a fixed mutable handle is rejected.

References require initializers and cannot contain `none` or `null`. References
to shorter-lived storage cannot escape into outer bindings. A reference result
must declare the checked-reference or checked-slice parameter that anchors it
with `<- parameter`; every return is verified against that source and the loan
is propagated into the caller. Raw/reference conversions, reference
comparisons, direct references to strings or views, and nested reference
storage are rejected in this first version.
`&*T` borrows a raw-pointer slot; accessing the raw pointee still requires unsafe
permission. See [checked references](docs/checked_references.md) for exact rules,
limits, and diagnostics. Integer literals gain contextual integer types with
representability checks; this does not convert already typed values.

Pointer casts, arithmetic, ordering, and `volatile` remain future features.

### 13.1 Fixed arrays

Fixed arrays are implemented as inline, contiguous values with length in the type:

```flecha
[3]i32 values = [10, 20, 30];
zeros := [0; 3];
[3]i32 empty_values = none;
values[0] = 42;
const [2]u8 fixed = [1, 2];
element := &values[1];
```

`[N]T` requires a nonnegative integer literal length. List initializers must match
it; repeat initializers evaluate their element once. Arrays copy on assignment,
argument passing, and return. Supported elements are scalars, raw pointers, and
nested arrays. Reference handles and string/view elements remain unsupported.

`.len()` returns `usize`. Indices must be `usize`; integer literals are contextual.
Statically known invalid indices are rejected and other indices have runtime
bounds checks, including inside unsafe blocks. `const [N]T` prevents mutation.
Whole-array and element references use the existing checked borrowing rules;
an element borrow conservatively borrows the entire array. See
[fixed arrays](docs/fixed_arrays.md) for full rules and limitations.

### 13.2 Iteration

```flecha
for index := range values {}
for (index, value) := range values {}
for (u32 index, i32 value) = range values {}
loop usize index = 0; index < values.len(); index++ {}
```

Range evaluates and copies a fixed-array value once, then yields ascending indices
and optional element copies. Inferred indices use `usize`; explicit integer index
types must represent every index. `_` discards a binding. Bindings are local to
iteration. Counted loops execute their initializer once and their update after
each body execution, including `proceed`; `break` skips the update. Postfix `++`
works on writable integer locations, evaluates the location once, and yields its
previous value. See [iteration](docs/iteration.md).

### 13.3 Checked slices

Slices are implemented as borrowed addresses and `usize` lengths:

```flecha
[]i32 mutable_view = values[1:3];
[]const i32 readonly_view = mutable_view[:];
const []i32 fixed_handle = values[:];
```

`start:end` is half-open; omitted bounds default to zero and receiver length.
Slices neither allocate nor own their elements. Bounds are checked even in unsafe
blocks. `len()`, indexing, `front()`, and `back()` are supported; endpoint methods
are writable locations when elements are mutable. Mutable handles move, shared
handles copy, and calls reborrow. Slice ranges take a shared reborrow rather than
copying the backing elements. Slice results use the same `<- parameter>`
contract as reference results. A flat result tuple may contain multiple
references or slices from that one source when it is immediately destructured;
overlapping mutable results are rejected. Nested borrowed storage remains
unsupported. See [checked slices](docs/checked_slices.md) and
[reference and slice returns](docs/ref_and_slice_returns.md) for exact rules,
limitations, and diagnostics.

### 13.4 Structures

```flecha
structure Box {
    i32 x;
    private { i32 y; }
};
Box Box::init() override {
    return build Box { self.x = 2; self.y = 40; };
}
i32 Box::value(&const self) { return self->x + self->y; }
```

Structures, public/private fields and methods, generated `init` and `zero`,
explicit `build`, and compiler-only `tear` hooks are implemented. Values use
`box.value()` and `box.x`; references and raw pointers use `box->value()` and
`box->x`. Static calls use `Box::method()`. Lifecycle overrides require
`override`; custom teardown precedes reverse field destruction at scope exit.
An unsafe tear makes a type immovable and requires unsafe construction. Fresh
factory results construct directly in their final destination. See
[structures](docs/structures.md) for copy rules, borrow checking, supported
fields, construction limits, examples, and diagnostics.

## 14. Memory and runtime model

Flecha should not require a garbage collector. Stack allocation is the ordinary default. The implemented explicit raw allocation API is `new<T>(value)` returning `(*T, error)`, paired with unsafe `free(ptr)`. It uses the system allocation service; custom allocators remain future work.
Runtime-sized scalar and supported trivially copyable structure storage uses
`new<[]T>(count)`, returning a move-only `*[]T` owner whose `{data, length}`
representation supports checked unsafe indexing without allocator queries. See
[dynamic arrays](docs/dynamic_arrays.md) for the implemented limits and the
Flecha `RawArray<T>` library and its safe `Array<T>` wrapper.
On failure, new returns null and `AllocationError.OutOfMemory` without evaluating
the initializer. Free runs typed teardown and accepts null. See
[raw allocation](docs/raw_allocation.md) for the unsafe contract and current limits.
Mandatory allocation analysis rejects possible leaks, double frees, use-after-free,
known invalid frees, tracked null dereferences, and ownership effects the current
checker cannot verify. Unsafe permission does not bypass these checks; unknown
external pointer validity still requires a caller contract.

Raw-pointer contracts use `lend *T` parameters, consuming `own *T` parameters,
`own *T` returns (including tuple elements), and `own *T` structure fields.
References never take either qualifier. Ownership remains separate from const
and nullability. See [ownership contracts](docs/ownership_contracts.md) for
transfer, teardown, and initial implementation restrictions.

The language should avoid hidden allocation in:

- tuple construction;
- error returns;
- string and slice operations;
- generic specialization;
- ordinary function calls.

Owning strings have deterministic lexical destruction. `defer function(args);`
captures arguments at registration and runs the call when its current lexical
scope exits, before that scope destroys its owned strings and structures. Calls run in reverse
registration order on fallthrough, return, break, proceed, and outward jumps.
Return expressions evaluate before cleanup; traps do not unwind. String/view
captures are independent snapshots, and reference/slice captures retain their
loans until scope exit. Only direct function calls are currently supported. See
[deferred calls](docs/defer.md) for the full contract and examples. General
ownership and allocator cleanup remain future work.

Flecha should support freestanding programs that do not link a language runtime.

## 15. Data layout and interoperability

Flecha should eventually provide:

- predictable struct, tuple, and enum layouts;
- explicit size and alignment inspection;
- packed and explicitly aligned representations;
- volatile memory access;
- native function pointers;
- C-compatible calling conventions;
- importing and exporting C-compatible symbols.

Layout must not be inferred solely from the behavior of a particular backend. Language-level rules will define which layouts are guaranteed and which remain implementation-defined.

## 16. Compilation model

The reference compiler is implemented in C++23.

The implemented compilation pipeline is:

```text
Flecha source
    -> lexer
    -> parser
    -> abstract syntax tree
    -> semantic and type analysis
    -> lowered bound program
    -> LLVM backend
    -> object file
    -> native linker
    -> executable
```

The LLVM backend emits textual IR or a host-native object. The native driver can
link that object with the version-matched runtime and explicit native inputs.
The compiler does not include a C source backend, optimization-level selection,
source-level debug information, or explicit cross-compilation in the 0.1 line.
See [LLVM code generation](docs/llvm_codegen.md) and
[native code generation](docs/native_codegen.md).

The compiler should eventually be self-hosted in Flecha, but self-hosting is not an initial milestone.

## 17. Modules and tooling

Each `.fl` source unit declares `module name;`. Every source unit with the same
declared name in the same namespace and source identity contributes to one
logical module, regardless of filename. Those units share declarations and
nominal identity, while their `@imports` bindings remain source-local. Import
blocks use `@imports :: root::path::module :: lib::library::module as alias;`,
with a multiline form preferred. `root` and `lib` are reserved in import
paths.

`private module name;` remains accepted with a deprecation warning and
restricts importing the complete logical module to its quiver. All contributing
source units must agree on private status. `export` always has global
declaration visibility, including inside a legacy private module; the module
import gate still controls whether an outside importer can reach it.

Top-level declarations use `export` for access from every importing module,
`quiver` for access from modules in the same immediate directory and the same
source-root or library identity, and no modifier for module-only access.
Wildcard imports do not bypass these boundaries. Exported and quiver
structures and enums bundle non-private member functions; member functions do
not take declaration-visibility modifiers, and private members remain limited
to methods of the exact owning type. Exported APIs may expose only exported
nominal types. Quiver APIs may also expose quiver types from the same quiver,
but never module-private types.

Directory imports use `root::path::*` for immediate declared logical modules or
`root::path::{first, second}` for selected declared module names. Distinct
same-named imports overlap under one local namespace, with collisions diagnosed
at the member use site; explicit aliases provide isolation. Repeated imports of
the same canonical logical module are deduplicated. See
[modules](docs/modules.md), [logical module composition](docs/module_composition.md),
and the [0.2.1 module correction](docs/release_0_2_1.md).

Longbow provides project creation, local checking/building, and execution:
`longbow new hello`, `longbow check`, `longbow build`, and `longbow run`.
See [Longbow](docs/longbow.md) for its manifest, library scaffolds, and current
limits. The command set below is a historical tooling roadmap, not the current
compiler CLI.


The intended command-line interface is:

```text
flecha build main.fl
flecha run main.fl
flecha check main.fl
flecha fmt main.fl
flecha test
```

Flecha tooling should eventually include:

- an official formatter;
- an editor language server;
- source-level diagnostics;
- go-to-definition and symbol navigation;
- completion and hover information;
- semantic highlighting;
- documentation generation;
- a build and package model;
- cross-compilation support.

Editor support should be developed in layers:

1. a TextMate grammar for immediate highlighting of `.fl` files;
2. a Tree-sitter grammar for structural parsing and highlighting;
3. Language Server Protocol semantic tokens driven by the compiler's type information.

Important initial highlighting categories include function modifiers, control-flow keywords, primitive types, type names, function names, parameters, variables, properties, numeric literals, strings, comments, and the built-in constants `none`, `null`, `true`, and `false`.

## 18. Illustrative program

The following example uses the implemented fixed-array and checked-slice syntax:

```flecha
void fill([]u8 destination, u8 value) {
    loop usize index = 0; index < destination.len(); index++ {
        destination[index] = value;
    }
}

i32 main() {
    [4]u8 values = none;
    fill(values[:], 7);
    if values.front() != 7 || values.back() != 7 {
        return 1;
    }
    return 0;
}
```

## Generics

Generic functions, structures, and callable signatures use explicit type
parameters and arguments:
`T identity<T>(T value)`, `structure Box<T> { T value; };`, and
`func Mapper<T>(T) => (T);`. Methods inherit owner parameters through
`Box<T>::method`. Structure specializations have distinct nominal identities;
callable specializations remain structural. All specializations are checked on
use.
References remain valid function parameters and cannot be stored in structure
fields. Existing ownership, const, visibility, and lifecycle rules apply.
See [generics](docs/generics.md) for the implemented rules and limits.

Generic parameters may use closed compile-time type-set constraints, as in
`T identity<T: numeric>(T value)`. Built-in numeric sets and custom
`typeset name { members, };` declarations are supported. `T.isof<name>` folds
to a boolean during specialization. Type sets have ordinary module visibility
and no runtime representation. See [type sets](docs/type_sets.md).

## 19. Open design questions

The following decisions are intentionally not finalized:

- re-exports, cyclic modules, and separately compiled module interfaces;
- additional declaration forms, including whether to introduce `var`;
- string literal representation and encoding;
- multi-source, heap-owner, and other more general borrowed-result contracts;
- union syntax;
- partial aggregate moves and cleanup rules for future owning types;
- allocator interfaces;
- arithmetic overflow behavior;
- floating-point equality involving NaN and signed zero;
- compile-time execution and metaprogramming;
- tuple layout and ABI rules;
- optional values distinct from primitive zero;
- error propagation syntax;
- generator and coroutine syntax using the reserved `yield` keyword;
- concurrency and memory-model rules;
- cross-compilation targets and stable aggregate calling conventions;
- whether `null` remains a separate pointer spelling or is replaced entirely by `none`.

These questions should be resolved by prioritizing predictable behavior, a small implementation surface, useful diagnostics, and compatibility with low-level programming.

## 20. Implementation status

The current compiler implements source-located diagnostics; the lexer, parser,
semantic and ownership analyses; lowering; and LLVM IR, object, and native
executable emission. Implemented language areas include functions and control
flow, primitives and conversions, `none`, tuples, errors, enums and matching,
strings and views, raw pointers and unsafe blocks, checked references, fixed and
runtime-sized arrays, checked slices, structures and lifecycle hooks, explicit
generics, callable values and closures, single-use callables, raw allocation,
ownership contracts, and direct, indirect, and member deferred calls. Modules
support global, quiver, and module-private visibility. Flecha and Longbow also
discover and run logical `<name>_test` modules through first-class `test`
declarations. Test module identity comes from the declaration rather than the
filename, and several source units may contribute tests to one module. The
linked feature documents define the exact supported forms and diagnostics.

The [Flecha 0.3.0 milestone](docs/release_0_3.md) expands the standard library
and its narrow runtime foundations without implying that these larger
milestones are complete. Stable aggregate ABIs,
cross-compilation, debug information, a formatter and language server, package
registries, broader resource ownership, and self-hosting remain future work.

## 21. Guiding principle

Flecha should make low-level programming direct without making dangerous behavior invisible. Its defining principle is:

> Mechanically predictable code with locally visible costs.

New features should be evaluated against that principle before they are added to the language core.
