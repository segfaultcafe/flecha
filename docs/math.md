# Math

Import the foundational math module with:

```flecha
@imports :: lib::std::math::ordering;
```

The module initially provides generic ordered-value operations:

```flecha
smaller := ordering::min<i32>(left, right);
larger := ordering::max<i32>(left, right);
limited := ordering::clamp<i32>(value, lower, upper);
```

`min` returns the right value only when it compares less than the left value.
`max` returns the right value only when the left value compares less than it.
Both therefore preserve the left value when the values compare equal.

`clamp` limits a value to an inclusive lower and upper bound. The caller must
provide an ordered range where `lower <= upper`. It returns the lower bound when
the value is below the range, the upper bound when the value is above the range,
and the original value otherwise.

The functions are checked when instantiated and support types for which the
required `<` operation is valid. Numeric type arguments remain explicit under
Flecha's current generic rules. Floating-point calls inherit the language's
comparison behavior; code requiring consensus-stable behavior should reject
NaN before calling these functions until NaN ordering is fully specified.

Checked, wrapping, and saturating arithmetic and bit operations belong to
separate sibling modules in `std::math`, as planned by the
[Flecha 0.3.0 milestone](release_0_3.md).
