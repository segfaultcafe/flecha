# Function overloading

Flecha functions may share a source name when their ordered parameter types
are different. Overloading applies to free functions, module-private functions,
and static and instance methods.

```flecha
i32 parse(i32 value) { return value; }
i64 parse(i64 value) { return value; }

structure Counter { i32 value; };

i32 Counter::add(&const self, i32 amount) {
    return self->value + amount;
}

i64 Counter::add(&const self, i64 amount) {
    return self->value.cast<i64> + amount;
}
```

The return type is not part of an overload's identity. Two declarations with
the same name and parameter types are duplicates even when they return
different types. Parameter names also do not distinguish overloads.

## Call selection

At a call, Flecha first prefers an exact parameter-type match. It then
considers the language's ordinary assignment and reborrow conversions. A call
is rejected when no candidate accepts its arguments or when multiple best
candidates remain. Return context never selects an overload.

```flecha
i32 width(stringv value) { return value.len().cast<i32>; }
i32 width([]const u8 value) { return value.len().cast<i32>; }

textWidth := width("flecha");
```

An overloaded name must currently be selected by a direct call. It cannot be
used by itself as a callable value because there is no target-type-based
overload selection.

Default arguments do not change a declaration's parameter signature. They can
therefore make a call ambiguous when more than one overload accepts the
omitted arguments.

## Safety and foreign functions

Every function in one overload set must have the same safety mode. A safe and
an `unsafe` function cannot share a name in the same scope. A set containing
only unsafe overloads is valid, and every selected call still requires an
unsafe context.

`extern` declarations cannot be overloaded. A foreign declaration names one
stable ABI symbol, while Flecha overloads receive compiler-generated native
link names. Use distinct Flecha and foreign symbol names, or place a set of
ordinary Flecha wrapper functions in front of the foreign boundary.

Crashable and non-crashable functions remain separate effect spellings:
`operation()` and `operation!()` are not selected from one overload set.

## String views as byte input

`stringv` explicitly casts to a borrowed immutable byte slice without an
allocation or copy:

```flecha
[]const u8 bytes = text.cast<[]const u8>;
```

The result has the same lifetime as the source view. Casting to mutable
`[]u8` is rejected. This conversion lets APIs provide `stringv` convenience
overloads while keeping their byte-slice implementation canonical:

```flecha
Digest hash([]const u8 input) { /* implementation */ }
Digest hash(stringv input) {
    return hash(input.cast<[]const u8>);
}
```
