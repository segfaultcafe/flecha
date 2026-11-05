# Workspace Guard

`workspace_guard` is Flecha 0.3.0's command-line north-star example. It creates
portable SHA-256 manifests for directory trees, verifies a tree against an
existing manifest, compares manifests, and runs a child process only after a
successful verification.

```text
longbow run -- snapshot <root> <manifest>
longbow run -- verify <root> <manifest>
longbow run -- diff <old-manifest> <new-manifest>
longbow run -- run <root> <manifest> -- <program> [arguments...]
```

For `snapshot` and `verify`, the root may be omitted when
`FLECHA_GUARD_ROOT` is set.

The manifest contains no absolute paths, timestamps, permissions, or native
filesystem identifiers. Entries are sorted by normalized `/`-separated
relative paths. Digests and paths are lowercase hexadecimal, making the format
unambiguous and byte-for-byte deterministic across supported hosts. Store the
manifest outside the scanned root so it does not become part of its own tree.

The example deliberately rejects symbolic links, unsupported filesystem entry
types, traversal components, and filenames that are not portable between
Linux, Windows, and macOS. The manifest destination must not already exist;
failed writes remove their partial output.

Stable command exit codes are `0` for success, `2` for usage, `3` for an I/O,
environment, allocation, or spawn failure, `4` for an invalid manifest, `5`
for a workspace mismatch, `6` for an unsupported entry, and `7` for abnormal
child termination. A normally exited child started by `run` contributes its
own exit code directly.

This is an integration and release-qualification fixture, not a supported
backup or security product.
