# Math

The foundational numeric and integer modules are imported separately:

```flecha
@imports
    :: lib::std::math::math
    :: lib::std::math::integer
    :: lib::std::math::bits
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

## Bit operations

The `bits` module provides portable operations for the `unsigned_integer` type
set:

```flecha
bits := bits::width<u32>();
ones := bits::count_ones<u32>(value);
zeros := bits::count_zeros<u32>(value);
leading := bits::leading_zeros<u32>(value);
trailing := bits::trailing_zeros<u32>(value);
left := bits::rotate_left<u32>(value, count);
right := bits::rotate_right<u32>(value, count);
reversed := bits::swap_bytes<u32>(value);
mirrored := bits::reverse<u32>(value);
power := bits::is_power_of_two<u32>(value);
high := bits::highest_one<u32>(value);
low := bits::lowest_one<u32>(value);
lowerPower := bits::bit_floor<u32>(value);
upperPower, ceilError := bits::checked_bit_ceil<u32>(value);
```

Zero has a full type width of both leading and trailing zeros. Rotation counts
are reduced modulo the width, so zero, full-width, and larger rotations are
defined. `width<usize>()` follows the target pointer width.
`highest_one`, `lowest_one`, and `bit_floor` return zero when given zero.
`checked_bit_ceil` maps zero to one and reports `ArithmeticError.Overflow` when
the next power of two is not representable in the input type.
