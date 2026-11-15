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

The interface call `value.readJson(&reader)` is verified through its contract.
Provider bodies remain independently checked. The compiler accepts managed
values and references whose signatures do not expose raw allocation handles;
raw pointer effects without a supported summary still report `FLECHA-S0052`.
Mutable interface calls invalidate tracked owning-field aliases conservatively,
including calls scheduled with `defer`.

The probe passes against json commit
`558a2db9d6bbd2782481f88e989efa89cb36f1d4`. It expects exit code 42 after checking
nested objects, owned strings read by value, booleans and integers, renamed and
explicit-only fields, skipped private fields, unknown properties, malformed
input, type mismatch, and trailing data. The independent
`expansion/reader_protocol.fl` fixture covers a protected helper, runtime loop,
type selection, constrained factory, and a reader owning a raw allocation, so
CI exercises the ownership boundary without an external library checkout.

This does not establish parity for every supported JSON field type, allocation
failure, or serialization.

The public contract requires only `readJson`. Its behavior expands field
selection inside the runtime property loop, without a protected helper slot.
`Manual` implements only `readJson`. Compile-time field requirements and unique
name validation run before the runtime loop. The helper-based fixture above
remains coverage for ordinary protected interface dispatch, not a JSON API
requirement.
