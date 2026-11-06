# Flecha 0.3.1 JSON slice bug fix

Flecha 0.3.1 corrects generated JSON methods for structure fields whose type
is a slice. Flecha 0.3.0 accepted the JSON array protocol in `Reader` and
`Writer`, but its derived methods did not emit array serialization or
deserialization for slice fields.

## Corrected behavior

Generated writers serialize `own []T` fields as JSON arrays, and generated
readers deserialize arrays into `own []T` fields. The
element contract is recursive, covering primitive values, owning strings,
nested owning slices, and structures decorated with the same JSON Reader and
Writer.

```flecha
@json(structure)
structure Response {
    own []string names;
    own []own []i32 matrix;
};
```

Deserialization requires an owning destination because decoded array storage
must outlive the Reader. Borrowed slice fields (`[]T` and `[]const T`) remain
outside the derived JSON contract. A Reader-backed decoration on an owning
slice whose element type contains a borrowed slice or `stringv` is a
compile-time error.

Partially decoded arrays are destroyed on element parse failure or allocation
failure. Allocation failure is returned as `AllocationError.OutOfMemory`; no
partially initialized owner escapes.

## Upgrade

Install the 0.3.1 compiler and matching VS Code extension. No Flecha source
change is required for an existing `own []T` JSON field. Projects that used a
borrowed destination must change it to `own []T` or provide an explicit
`readJson` override with application-managed storage.

The fix uses the existing JSON array methods (`beginArray`, `endArray`, and
`nextArrayElement`), so it does not introduce a new public package protocol.

## Validation

The release suite covers empty and populated string slices, nested owning
slices, slices of decorated structures, writer round trips, element parse
failure after partial initialization, precise borrowed-destination diagnostics,
and injected failure at every allocation performed while growing a decoded
slice. The allocation tests audit all owners for leaks and double frees.
