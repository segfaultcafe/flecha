# Reference and slice returns

Status: implemented on `feat/ref-and-slice-returns` for the next Flecha
language version.

## Contract syntax

A function whose result contains a checked reference or checked slice declares
the parameter that anchors those returned loans with `<-`:

```flecha
&const i32 value(&const self) <- self {
    return &self->value;
}

[]const i32 tail([]const i32 values) <- values {
    return values[1:];
}
```

`<-` is a single Flecha token. The contract appears after the parameter list
and before the function body. It is compile-time information and does not alter
the native representation of references, slices, or functions.

The named source must be a checked-reference or checked-slice parameter. A
method receiver is an ordinary source parameter named `self`. Raw pointers,
owning pointers, value parameters, locals, and temporaries cannot anchor a safe
borrowed result. An `unsafe` context does not relax these rules.

Every reference and slice result requires a contract. A contract on a result
that contains no borrowed value is an error. The first version names exactly
one source; contracts such as `<- (left, right)` remain future work.

## Result rules

The result may be the source handle itself or a checked reborrow derived from
it, including a structure field, fixed-array element, fixed-array slice,
slice element, or subslice. Every reachable return path must derive every
borrowed result from the declared source. Locals, temporaries, and another
parameter are rejected even when their types match.

Access can be weakened but never strengthened:

```text
&T       -> &T          allowed
&T       -> &const T    allowed
&const T -> &const T    allowed
&const T -> &T          rejected

[]T       -> []T          allowed
[]T       -> []const T    allowed
[]const T -> []const T    allowed
[]const T -> []T          rejected
```

The returned loan is substituted onto the caller's actual argument. Existing
last-use, exclusivity, moved-handle, parent-reborrow, field-projection, and
scope rules then apply. The source cannot be moved, rebound, destroyed, or
accessed incompatibly while a returned loan is live.

Calls compose transitively: a function may return the result of another
borrowed-result function when the inner result is ultimately anchored to the
outer function's declared source. Contracts are retained across modules and
generic specialization. Recursive borrowed-result calls use the declared
source conservatively.

## Multiple results

A single source contract applies to every borrowed element of a flat tuple
result:

```flecha
(&i32, &i32) fields(&self) <- self {
    return (&self->left, &self->right);
}

(&const i32, usize) first_and_length(&const self) <- self {
    return (&self->first, self->length);
}
```

Each borrowed element is checked independently. Shared results may overlap.
Mutable results must be disjoint; returning the same mutable place twice, or a
mutable place overlapping a shared result, is rejected.
Distinct structure-field projections are proven disjoint. Array and slice
elements remain conservative whole-sequence loans, so multiple mutable element
results are rejected until index-disjointness analysis exists.

In this milestone, a tuple containing borrowed results must be consumed by
immediate destructuring. Every destination becomes an independent reference or
slice handle with its own origin, projection, mutability, movement, and
last-use state. Storing, nesting, returning from an uncontracted function,
passing as an ordinary aggregate argument, or defer-capturing a borrowed tuple
is future work. A contracted function may directly forward a compatible
borrowed tuple from another contracted call.

## Conservative boundaries

This feature does not create checked references or slices from raw pointers.
Safe borrowing from `Array<T>` and other heap owners requires a separate
owner-anchored heap-borrow operation. It also does not add returned `stringv`
contracts, borrowed structure fields, external C ABI references, multiple
contract sources, or separately compiled module interfaces.

## Implemented actions

### Flecha language surface

1. Define `<- source` as part of function declaration syntax without adding a
   lifetime keyword or changing the ABI.
2. Require the contract on every borrowed result and define permission,
   single-source, and return-origin rules.
3. Support flat multiple borrowed results only through immediate
   destructuring, with overlapping mutable results rejected.
4. Update the specification, feature guides, changelog, VS Code grammar, and
   Neovim syntax.

### C++ compiler core

1. Add the `<-` token, parser recovery, and AST contract source/range.
2. Resolve the source to a parameter index during function registration and
   retain it in `FunctionSymbol` across modules and generic expansion.
3. Validate contract presence, result shape, source type, access compatibility,
   and each return expression before lowering.
4. Substitute caller origins into free and member-call results while retaining
   parent handles, last-use restrictions, and conservative sequence loans.
5. Propagate one tuple-aware borrow value per destructured destination and use
   validated abstract result projections for disjoint mutable multi-results.
6. Preserve the existing LLVM reference and slice representations and verify
   direct, transitive, generic, module, multi-result, and native behavior.

## Definition of done

- All valid direct reference and slice contracts compile and execute natively.
- Invalid or missing contracts fail before lowering with precise source ranges.
- Locals, temporaries, wrong parameters, raw storage, and permission
  strengthening are rejected.
- Returned loans protect caller storage through branches, loops, calls, and
  last-use analysis.
- Direct, member, module, generic, and transitive calls preserve the contract.
- Flat multiple borrowed results work through immediate destructuring, retain
  independent projections, and reject overlapping mutable results.
- No invalid program reaches LLVM generation; generated modules verify.
- Parser, AST, semantic, lowering, codegen, native, module, generic, editor,
  diagnostic-code, and sanitizer coverage is present.
- The full CTest suite, focused native tests, editor tests, and
  `git diff --check` pass; protected CI remains the release gate for sanitizer
  and platform coverage.

## Acceptance matrix

Positive coverage includes mutable and shared references, mutable and shared
slices, permission weakening, receiver fields, fixed arrays, subslices,
last-use release, nested calls, modules, generic specializations, recursion,
and mixed value/borrowed tuple results.

Negative coverage includes missing or duplicate contracts, unknown or
non-parameter sources, non-borrowed and raw-pointer sources, contracts on value
results, local and temporary escapes, wrong-source returns, permission
strengthening, conflicting mutable results, stored borrowed tuples, invalid
generic specializations, and source mutation/movement/destruction while a
returned loan remains live.
