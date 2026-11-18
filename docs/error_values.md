# Error values

Implemented on `feat/error-values`. Error values are allocation-free, copyable
values. Functions return them directly or in tuples; callers inspect and
propagate them explicitly. This feature introduces no exceptions or unwinding.

## Named families and generic error

```flecha
error MathError {
    DivisionByZero,
    Overflow,
}

error InputError { InvalidDigit }

MathError specific = MathError.Overflow;
error general = specific;
general = InputError.InvalidDigit;
```

An error declaration is top-level and contains one or more distinct failure
names. Commas separate names; a trailing comma is optional. Variants have no
payloads or explicit numeric assignments. `none` is implicit and cannot be
declared as a variant. Families are visible throughout their compilation unit,
including in function signatures that precede their declarations.

A named family is a distinct type. A `MathError` accepts only its own variants,
other `MathError` values, or `none`. Built-in `error` accepts every family and
preserves the family and variant identity. Named-to-generic conversion happens
in assignments, arguments, returns, tuple fields, and destructuring. Generic-to-
named conversion and conversion between different named families are rejected.
There are no numeric casts, downcasts, or error-to-string conversions. A cast
to the identical type is allowed, as for other scalar types; widening is implicit.

Type names occupy the same namespace as functions and variables. A local can
shadow a family name, making that family unavailable by name in that scope.
A type name alone is not a value: use `MathError.Overflow`. Variant selection
through an instance (`err.Overflow`) is rejected.

## Success, comparison, and initialization

```flecha
MathError namedSuccess = none;
error success = namedSuccess;
success == none;                  // true
MathError.Overflow == none;        // false
success != MathError.Overflow;     // true
```

`==` and `!=` compare variants within a family. If either operand is generic
`error`, the other error operand widens before comparison. Different families
remain distinguishable even if their variants have identical names or internal
codes. Success from any family converts to the same generic success value.
Comparing two different named families directly is a type error; widen them to
`error` if that comparison is intended.

Strict `===` and `!==` compare static type and value without widening or
contextual conversion. A named error and a generic error are strictly unequal,
even if their ordinary values compare equal. Errors from distinct named families
are strictly unequal. An error success value is strictly unequal to literal
`none`. Operands still execute when their differing types determine the result.

Errors do not support arithmetic, ordering, increment, Boolean operators, or
implicit truth tests. Write `if err != none`, not `if err`.

```flecha
error initialized = none;
error pending;             // allowed; must be assigned before any read
pending = MathError.Overflow;
const error fixed = pending; // cannot be reassigned
```

The existing definite-assignment, const, unsafe-pointer, and borrowing rules
apply. Error values can be stored in fixed arrays and tuples and accessed through
checked slices and references. A copied error has no resource ownership.

## Multiple results and explicit propagation

```flecha
(i32, MathError) divide(i32 a, i32 b) {
    if b == 0 { return (0, MathError.DivisionByZero); }
    return (a / b, none);
}

(i32, error) calculate(i32 a, i32 b) {
    return divide(a, b);
}

// Inside a function:
quotient, err := divide(17, 5);        // err has type MathError
i32 value, error failure = divide(1, 0);
```

Tuple construction, return, assignment, and destructuring keep their existing
single-evaluation rules. No compiler rule associates the first result's validity
with the error result: the function and caller define that contract. Ignoring an
error is permitted. Explicit `try` propagation is described below. Deferred calls
capture error arguments by value at registration.
Returning a failure runs normal scope-exit defers just like any other return.

## Explicit propagation with `try`

`try` is a prefix expression on an ordinary call returning `error`, a named
error family, or a tuple whose final field is one of those error types:

```flecha
(i32, error) calculate(i32 a, i32 b) {
    quotient := try divide(a, b);
    return (quotient, none);
}

// Error-only calls yield no value on success.
try save();
// Multiple success fields remain available for destructuring.
first, second := try read_pair();
// Crashability is independent: the call still requires !.
array := try parse_array_string!(text);
```

Failure-only work can be attached with `peek` without changing the success
value of the `try` expression:

```flecha
// No error binding is introduced.
try save() peek { increment_failure_count(); };

// The exact returned error type is inferred.
value := try parse(text) peek err { stderrln(err); };

// An explicit compatible error type can widen the binding for translation.
value := try parse(text) peek err as error { err = normalize(err); };
```

The block runs only when the operand's error is not `none`. It is an ordinary
nested lexical scope and can read or mutate enclosing variables. If present,
the error binding exists only in that scope and is mutable; its value when the
block finishes is the value propagated by `try`. A `peek` block cannot contain
`return`, propagate another `try` or `.await?`, or use loop/jump control to
leave the block. Secondary fallible work must therefore inspect its result
explicitly. A bare block is not a hook: `try operation() {};` is invalid, while
the existing `try operation();` remains an immediate propagation statement.

`none` means success. For `(T, error)`, success yields `T`; for
`(A, B, ..., error)`, it yields the tuple `(A, B, ...)`. Nested tuples and errors
inside those values are preserved. For an error-only call, success yields no
value, so `try operation();` is a valid statement.

Failure immediately returns the final error from the enclosing function. That
function must itself return an error or a tuple with a final error field; its
error type must accept the propagated error. Named errors may widen to generic
`error`, but generic errors cannot narrow and distinct named families cannot
convert. Other return fields receive their ordinary `none` defaults, exactly as
in an explicit `return (none, ..., err)`. A field that cannot be defaulted (for
example a reference) makes this implicit return invalid. Default constructors
retain their ordinary effects and unsafe requirements.

The operand is evaluated once, in normal expression order. Failure skips the
rest of the expression and runs normal scope-exit defers and owned-value cleanup,
including values moved into earlier arguments or partially constructed
aggregates. Raw allocation ownership obligations remain the same as for an
explicit return. An ordinary call's success fields are real values even when
it returns an error, and managed fields are cleaned up on failure.

`try` is allowed in function bodies, including methods and closures. Like an
explicit early return, it cannot escape a `build` block. Default arguments have
no caller return context and cannot contain `try`. Calls with another result
shape, storing an error tuple and then applying `try` to that variable, and
postfix `ordinary_call()?` are rejected. Existing ownership rules still apply:
`try` cannot extract an immovable value from a returned tuple.

This is an ordinary branch and return, not exception handling or runtime stack
unwinding. `!` still denotes an unrecoverable crash. Async `.await?` is separate:
it forwards the async execution failure and does not require an error field in
the async function's completed result. Ordinary `try` inside an async function
returns a completed error value; it does not turn that value into an execution
failure. Async executable lowering preserves that separate outcome through the
single-threaded runtime ABI.

## Representation and validation

Named errors lower to a 32-bit variant code. Zero denotes success; nonzero codes
are assigned internally in declaration order. Generic errors lower to a pair of
family identity and variant code. Family identity uses a distinct module-private
global address whose address cannot be merged; widening a success code clears
that identity. Comparisons test both fields rather than aggregate padding.

These details are compiler-private, not public numeric values or a stable C ABI.
The current compiler processes one compilation unit; sharing family identities
across independently compiled units needs a future module/linkage contract.
Error storage has no allocation, destructor, message, or attached payload.

Diagnostics include `FLECHA-S0041` for an empty family, `FLECHA-S0042` for an
unknown type, and existing duplicate-name, invalid-member, incompatible-type,
invalid-operand, const, borrowing, and definite-assignment diagnostics.

`examples/error_values.fl` is executable and returns 42. Parser and semantic
tests cover syntax, resolution, conversion rules, qualifiers, and diagnostics.
LLVM and native tests cover family identity, success normalization, tuple
propagation, aggregates, references, raw writes, deferred captures, and strict
comparison side effects. Native harnesses expose only scalar test entry points.

The allocation built-in provides `AllocationError.OutOfMemory`, returned as
generic `error` by `new<T>`. See [raw allocation](raw_allocation.md).
