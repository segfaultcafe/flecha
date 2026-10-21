# Math

The foundational numeric and integer modules are imported separately:

```flecha
@imports
    :: lib::std::math::math
    :: lib::std::math::integer;
```

## Numeric operations

The `math` module accepts the built-in `numeric` type set:

```flecha
smaller := math::min<i32>(left, right);
larger := math::max<i32>(left, right);
low, high := math::min_max<i32>(left, right);
limited := math::clamp<i32>(value, lower, upper);
magnitude := math::abs<i32>(value);
direction := math::sign<i32>(value);
```

`min_max` returns its values in ascending order. `clamp` uses the same ordering
internally, so reversed bounds are accepted and normalized. `abs` and `sign`
also accept unsigned integers and floating-point values.

Floating-point calls inherit Flecha's comparison behavior. Code requiring
consensus-stable behavior should reject NaN before using ordering operations
until NaN ordering is fully specified.

## Integer operations

The `integer` module accepts the built-in `integer` type set:

```flecha
even := integer::is_even<i32>(value);
odd := integer::is_odd<i32>(value);
multiple := integer::is_multiple_of<i32>(value, divisor);
power := integer::pow<i32>(base, exponent);
divisor := integer::gcd<i32>(left, right);
multiple := integer::lcm<i32>(left, right);
```

`is_multiple_of` returns `false` for a zero divisor. `pow` accepts a `usize`
exponent and uses exponentiation by squaring. `gcd` and `lcm` ignore input signs
and return a nonnegative result when that result is representable. `gcd(0, 0)`
and any `lcm` call containing zero return zero.

These functions do not yet add overflow detection beyond primitive Flecha
arithmetic. In particular, `pow` and `lcm` may exceed their result type, and the
positive magnitude of a signed type's minimum value is not representable in
that same type. Callers must keep results within range until checked, wrapping,
and saturating arithmetic APIs define those cases explicitly.
