# Local validation

During implementation, use `make test-changed` to build and validate the changed
subsystems. The default comparison base is `master`; override `TEST_BASE` when
the task has another base. The runner includes committed branch changes,
staged/unstaged edits, and untracked files. Shared compiler/runtime changes and
unknown paths deliberately select the full suite.

Add a regression test explicitly when needed, for example:

```sh
make test-changed TEST_ARGS='--regression ^ParserTests\.'
```

Use `make test-plan` to inspect selection. `--changed` is a manual replacement
for Git change detection, intended for controlled experiments; do not use it to
hide other changes in ordinary task validation.

`make test` selects the complete suite and reuses eligible local successes.
`make test-full` executes the complete suite without reading or writing result
cache entries. CI always runs the latter policy before merge. Use full local
validation for integration-boundary work or when explicitly requested.

After the relevant checks pass, do not repeat them unless code, inputs, failures,
or unresolved concerns justify another run. Read the compact runner summary and
JSON report first; inspect detailed logs only for failures or investigation.
Do not run concurrent builds/tests in the same build directory outside the
runner. Keep sanitizer and release-qualification checks for applicable changes.

Runner changes require `make test-runner-self-test`. Selection is a conservative
development aid, not a replacement for the full CI gate. See
`docs/test_runner.md` for cache scope, invalidation, and benchmark methodology.
