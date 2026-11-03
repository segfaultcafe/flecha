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
streaming, full-block, and padding-boundary behavior.

SHA-2 provides integrity digests, not authentication or password hashing.
Callers must use HMAC for keyed authentication and a dedicated password
hashing construction for passwords. Comparing secret authentication values
also requires a constant-time comparison API.
