# Math

The foundational numeric and integer modules are imported separately:

```flecha
@imports
    :: lib::std::math::math
    :: lib::std::math::integer
    :: lib::std::math::errors;
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

## Checked arithmetic

Checked operations return the arithmetic result together with an
`errors::ArithmeticError`. An exact operation returns `none` as its error:

```flecha
sum, sumError := integer::checked_add<i32>(left, right);
difference, subError := integer::checked_sub<i32>(left, right);
product, mulError := integer::checked_mul<i32>(left, right);
quotient, divError := integer::checked_div<i32>(left, right);
```

Addition, subtraction, and multiplication report
`ArithmeticError.Overflow` when the mathematical result is outside the result
type. Division reports `ArithmeticError.DivisionByZero` for a zero divisor and
`ArithmeticError.Overflow` when a signed minimum value is divided by negative
one.

## Saturating arithmetic

Saturating operations preserve representable results and clamp overflow to the
nearest bound of `T`:

```flecha
sum := integer::saturating_add<i32>(left, right);
difference := integer::saturating_sub<i32>(left, right);
product := integer::saturating_mul<i32>(left, right);
```

Primitive `+`, `-`, and `*` continue to wrap at the integer width. In
particular, `pow` and `lcm` use those primitive operations and may wrap if their
mathematical result is outside `T`; callers that require another policy must
check their input range explicitly.
