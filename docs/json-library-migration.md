# Moving JSON serialization into Flecha

The Flecha 0.4 compiler no longer recognizes a special JSON decoration or emits
JSON serialization/deserialization methods. JSON encoding, decoding, allocation,
field selection, and validation belong to the external JSON library, written
in Flecha using ordinary interfaces, generics, and behaviors.

## Updating a consumer

Use a JSON library checkout/release that declares typed decoration contracts
and the `JsonWritable` / `JsonReadable` behavior. The legacy 0.3 library with
`export decoration json;` is incompatible with this compiler.

Keep the existing decoration applications and instance calls:

```flecha
@json(structure)
structure User {
    @json("display_name") string name;
    private string secret;
};

// Within a function:
// encoded, writeErr := user.json();
// decoded, readErr := json::from<User>(encoded[:]);
```

Change `User::fromJson(source)` to `json::from<User>(source)`. The type argument
selects the decoded result type; the source argument is a byte slice. There is
no compiler-generated static factory. `json`, `writeJson`, `readJson`, and
`fromJson` are ordinary method names whose contracts come from declarations.

A typed decoration without a behavior supplies metadata only. It does not
implicitly add serialization, conformance, or a dependency on types named
`Writer` and `Reader`. A bodyless decoration application is rejected with
`FLECHA-S0064` and a message requiring a typed application contract.

## Library policy and compatibility

The migrated JSON library supplies the established public-field and explicit
field-selection policies, renamed keys, and duplicate serialized-name checks.
Visibility and uniqueness use general `isPrivate`, `require`, and
`requireUnique` reflection operations. Their diagnostics now originate from
library behavior requirements and ordinary semantic checks; callers should not
rely on the exact wording or diagnostic code of legacy compiler JSON errors.

The library supports owning strings, scalars, nested decorated structures,
and owned-array fields, including nested arrays and recursive structures.
The `writeArray<E>` and `readArray<E>` helpers are ordinary library generics.
There is no private JSON append intrinsic or JSON-specific lowering.
Handwritten providers and inherited interface defaults follow the same rules
as other behaviors.

## Verification

The compiler test suite covers typed JSON metadata without implicit methods,
ordinary JSON method names, rejection of the retired declaration/intrinsic,
module-alias resolution, and native behavior-generated serialization calls.
The external JSON checkout carries its reader/writer, behavior, stress, and
adversarial suites. Run them with the compiler being qualified:

```sh
cd /path/to/flecha-libs/json
FLECHA_COMPILER=/path/to/flecha/build/flecha longbow test . --verbose
```

The external-library contract check verifies handwritten providers, duplicate
serialized names, decorated private fields, borrowed-array rejection, and the
retired static factory:

```sh
python3 tools/verify_json_library_contract.py \
  --compiler build/flecha \
  --json-root ../flecha-libs/json \
  --std-root std/src
```

The ownership audit additionally checks a recursive JSON document and every
observed runtime allocation-failure position with a GNU-compatible linker:

```sh
python3 tools/verify_owned_slice_json.py \
  --compiler build/flecha \
  --json-root ../flecha-libs/json \
  --std-root std/src \
  --runtime build/src/runtime/libflecha_runtime.a
```

This migration concerns serialization of Flecha program values. Tooling still
uses JSON for package graphs, analyzer/LSP messages, and registry protocols.
