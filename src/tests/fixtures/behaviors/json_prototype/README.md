# JSON reader migration probe

This is an experimental integration probe, not a completed library migration.
It uses the committed `json.fl`, `reader.fl`, and `writer.fl` from a separate
flecha-libs/json checkout. The runner stages a temporary module, replaces the
compiler-known decoration with a typed declaration, and adds the contracts and
behavior in this directory. It never changes that checkout or reads its
uncommitted source modifications.

Run from the compiler repository:

```sh
python3 tools/verify_behavior_json_prototype.py \
  --compiler build-tests/flecha \
  --json-root /path/to/flecha-libs/json \
  --std-root std/src
```

The selected library commit can be pinned with `--json-revision`.

**Current blocker:** compilation reaches `from<T>` but reports `FLECHA-S0052`
(`cannot verify ownership effects of this call`) for the interface call
`value.readJson(&reader)`. Extending interface ownership verification is a
separate decision; this PR does not bypass that check. The runner currently
exits unsuccessfully at compilation, so its runtime cases are not yet proven.
The independent `expansion/reader_protocol.fl` fixture validates the protected
helper, runtime loop, type selection, and constrained factory without the
real Reader's ownership requirements.

Once the blocker is resolved, this probe expects exit code 42 after checking
nested objects, owned strings read by value, booleans and integers, renamed and
explicit-only fields, skipped private fields, unknown properties, malformed
input, type mismatch, and trailing data. It does not establish parity for every
supported JSON field type, allocation failure, or serialization.

The protected virtual helper is part of the interface contract. A handwritten
`readJson` alone does not fulfill that extra slot; `Manual` explicitly provides
both methods. The migration must decide compatibility for existing handwritten
providers before adopting this contract in the public library.
