# Hashing

The `hashing` module provides non-cryptographic hashes for in-memory data
structures:

```flecha
@imports :: lib::std::hashing::hashing;

i32 value = 42;
usize numberHash = hashing::hash<i32>(&value);
usize textHash = hashing::hash_string("flecha");
```

`hash<T>` accepts a checked reference to an equality-comparable value. It
hashes scalar values directly and traverses arrays, tuples, and structures by
their logical fields. Structure padding is never read. Strings are hashed by
content, and `+0.0` and `-0.0` are normalized because they compare equal.
`hash_string` accepts a `stringv` directly because checked references to string
views are not part of the current reference model.

The compatibility guarantee is the one required by hash tables: two values of
the same type that compare equal produce equal hashes. The numeric hash value,
mixing algorithm, and collision pattern are implementation details. In
particular, hashes can vary with target word size, and hashes of values that
contain pointers are valid only while those allocations remain alive in the
current process.

These functions are not cryptographic and must not be used for passwords,
signatures, authentication, persistent identifiers, canonical file formats,
or adversarial input protection. Those uses require separate algorithms with
specified portable encodings and security properties.

`HashMap<K, V>::init()` uses `hash<K>` together with structural equality. A
custom equality policy must always be paired with a compatible custom hash:

```flecha
map := collections::HashMap<Key, Value>::init()
    .withHash(customHash)
    .withEqual(customEqual);
```

The underlying `__hash<T>` spelling is a reserved compiler intrinsic used by
the standard library. Application code should call the exported module API.
