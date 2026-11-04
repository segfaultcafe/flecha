# Cryptography

The `std::cryptography::sha2` module provides portable SHA-256 and SHA-512.
It is separate from `std::hashing`, whose structural hashes are not suitable
for cryptographic use.

```flecha
@imports :: lib::std::cryptography::sha2;

[3]u8 input = [0x61, 0x62, 0x63];
sha256Digest, sha256Error := sha2::sha256(input[:]);
sha512Digest, sha512Error := sha2::sha512(input[:]);

// The stringv overload is allocation-free.
textDigest, textError := sha2::sha256("abc");
```

The one-shot functions accept either `[]const u8` or `stringv` and return
fixed-size owning digest values. The string overload borrows the view's bytes
without copying or allocating. `bytes()`
borrows the digest's 32-byte or 64-byte representation in standard big-endian
order.

## Streaming

`Sha256` and `Sha512` support incremental input:

```flecha
hasher := sha2::Sha512::init();
updateError := hasher.update(firstChunk);
if updateError != none {
    return updateError;
}

updateError = hasher.update(secondChunk);
if updateError != none {
    return updateError;
}

digest, finalizeError := hasher.finalize();
```

`update` accepts empty input. `finalize` consumes the current logical hash
state and can succeed exactly once. Further calls to `update` or `finalize`
return `Sha2Error.AlreadyFinalized`. `Sha2Error.MessageTooLong` reports input
whose encoded bit length cannot be represented by the selected SHA-2 variant.

The implementations allocate no heap memory, invoke no runtime ABI, perform
no input/output, and do not block. Hashers and digests are ordinary owning
values and may be moved. Published SHA-2 conformance vectors cover one-shot,
streaming, full-block, padding-boundary, and one-million-byte behavior.

SHA-2 provides integrity digests, not authentication or password hashing.
Callers must use HMAC for keyed authentication and a dedicated password
hashing construction for passwords. Comparing secret authentication values
also requires a constant-time comparison API.

## HMAC

The `std::cryptography::hmac` module provides HMAC-SHA-256 and HMAC-SHA-512.
Keys and messages are explicit byte views; textual messages can be passed
allocation-free with `stringv.cast<[]const u8>`:

```flecha
@imports :: lib::std::cryptography::hmac;

tag, tagError := hmac::sha256(
    key[:],
    "authenticated message".cast<[]const u8>
);
if tagError != none {
    return tagError;
}

tagBytes := tag.bytes();
```

`HmacSha256Tag` and `HmacSha512Tag` are fixed-size owning values containing 32
and 64 bytes respectively. `bytes()` returns a borrow tied to the tag. HMAC
does not interpret keys as text and does not provide `stringv` key overloads.

Incremental authentication uses `HmacSha256::create(key)` or
`HmacSha512::create(key)`, followed by any number of `update` calls and one
successful `finalize` call. A compiler-generated default HMAC value is not
initialized with a key: its `update` and `finalize` operations return
`HmacError.NotInitialized`. Repeated finalization and updates after finalization
propagate `Sha2Error.AlreadyFinalized`.

The implementation allocates no heap memory, invokes no runtime ABI, performs
no input/output, and does not block. Keys longer than the underlying SHA-2
block size are hashed as required by HMAC. RFC 4231 vectors cover short keys,
chunked messages, and keys longer than the block size.

Key material is copied into private, fixed-size stack state while constructing
the inner and outer pads. Intermediate buffers are not exposed through the
public API. This version does not promise that key material is securely erased
from memory after use; such a guarantee requires the secure-memory boundary.

HMAC computes authentication tags but does not verify them. Do not compare
secret tags with an early-exit equality loop. Use
`std::bytes::sequence::constant_time_equal` for byte slices or
`std::text::strings::constant_time_equal` for byte-oriented string views.
