# Public community-package qualification fixtures

These fixtures prove the v0.3 package lifecycle without making recurring CI
runs mutate registry history:

- `leaf` is the transitive public dependency;
- `package` imports that dependency under its own `foundation` alias; and
- `consumer` selects `package` under the unrelated local alias `community`.

The two public versions are published once under the dedicated
`segfaultcafe/e2e-community-*` identities, locked by the consumer, and then
yanked. Clean-install tests reuse the immutable lock and archives. They verify
that a yanked release remains available to an existing lock, while the normal
Longbow tests prove that new unlocked selection excludes yanked versions.
