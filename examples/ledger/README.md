# Deterministic Ledger

`ledger` is Flecha 0.3.0's persistence north-star example. It creates an
append-only chain, links every block through SHA-256, reloads and validates the
complete file before appending, and rejects malformed, truncated, reordered,
or tampered storage.

```text
longbow run -- create <file>
longbow run -- append <file> <payload>
longbow run -- verify <file>
longbow run -- list <file>
```

`list` renders payloads as lowercase hexadecimal so arbitrary stored bytes are
unambiguous. Command-line appends receive the host argument's UTF-8 bytes.

## Canonical format

Every file starts with the 16-byte ASCII header `FLECHA-LEDGER\n1\n`. Each
record then contains, without padding:

| Field | Encoding |
| --- | --- |
| block index | 8-byte unsigned big-endian integer |
| payload length | 8-byte unsigned big-endian integer |
| previous block hash | 32 bytes |
| payload | exactly `payload length` bytes |
| block hash | 32 bytes |

The genesis block has index zero and an all-zero previous hash. Later indices
must be consecutive and their previous hash must equal the preceding stored
block hash. A block hash is:

```text
SHA-256(
    "FLECHA-LEDGER-BLOCK-V1\n" ||
    canonical index || canonical payload length || previous hash || payload
)
```

This domain-separated field encoding never hashes structure memory. It has no
host endianness, padding, timestamp, path, permission, or machine identifier,
so the same block sequence produces byte-for-byte identical files on every
supported target.

The published `alpha` genesis block digest is
`aba5ed27f8ee302525deccc60adfb950fd3d71699082bb3b5c0bcad6a36ec139`.
Appending `beta` produces head digest
`d4298ba679357d6f92c5ea1da21f8895a3e6d514b35e51b3e68081744d9f6b4a`.
The complete two-block file is 185 bytes with SHA-256
`8597adac0ae2a0efee59c082eb1ea4e68fa5752d2ab30bfdfc7d5b6743a95847`.

Stable command exit codes are `0` for success, `2` for usage, `3` for an I/O
or allocation failure, `4` for malformed or truncated storage, and `5` for a
broken index, link, payload hash, or stored digest.

The example deliberately validates the complete chain in memory and appends
one encoded record with an explicit flush. It demonstrates deterministic
encoding, hashing, persistence, and corruption detection; it is not a
transactional database, consensus protocol, cryptocurrency, or crash-safe
storage engine.

