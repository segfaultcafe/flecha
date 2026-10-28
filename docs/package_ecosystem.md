# Flecha package ecosystem

Flecha 0.3.0 distributes independently versioned libraries as source packages.
Longbow resolves, downloads, verifies, caches, and publishes those packages.
The Flecha compiler consumes a resolved package graph and never performs
network access.

## Package locations

A published library exists in three places with different responsibilities:

1. The author's Git repository is the development origin for commits, issues,
   and contributions.
2. The public Flecha registry stores package metadata and an immutable snapshot
   of every published source version.
3. Longbow's local cache stores verified archives and extracted source trees
   for builds and offline reuse.

Deleting a repository or moving a Git tag must not break an existing package
version. The registry therefore owns the published snapshot rather than
redirecting downloads to the repository.

The public service uses `packages.flecha-lang.org` for human-facing package
pages and `registry.flecha-lang.org` for Longbow's API. Archive storage is
content-addressed by SHA-256 and may be served through an object store and CDN;
the public URL does not define the physical storage provider.

## Identity and imports

Registry identities are scoped by an owner:

```text
flecha/json
flecha/http
alexbsec/blockchain
acme/database
```

The `flecha` owner is reserved for official project packages. User and
organization owners publish community packages through the same registry and
protocol. Official status is registry metadata, not a separate resolver.

A dependency key is a local Flecha identifier and becomes the import alias:

```toml
[dependencies]
json = { package = "flecha/json", version = "^1.2.0" }
chain = { package = "alexbsec/blockchain", version = "^0.1.0" }
```

```flecha
@imports
    :: lib::json::parser
    :: lib::chain::blockchain;
```

Canonical owner/package identities never appear in Flecha import paths. This
lets callers choose concise aliases and consume same-named packages together:

```toml
[dependencies]
json = { package = "flecha/json", version = "^1" }
experimental_json = { package = "another_owner/json", version = "^0.2" }
```

## Standard library

`std` remains a version-matched component of the installed Flecha toolchain and
is injected automatically by Longbow as `lib::std`. It is not an ordinary
registry dependency. A public registry package cannot replace `std`.

An explicit local path override remains supported for compiler and standard
library development:

```toml
[dependencies]
std = { path = "../flecha/std" }
```

Registry packages declare the Flecha toolchain range they support and receive
the compatible bundled standard library automatically:

```toml
[package]
name = "json"
registry = "flecha/json"
version = "1.2.0"
kind = "library"
flecha = ">=0.3.0, <0.4.0"
```

The registry may mirror standard-library sources for browsing and archival
purposes later, but ordinary dependency resolution never selects `std` from
that mirror.

## Dependency sources

Longbow supports three dependency sources in 0.3.0:

```toml
[dependencies]
local = { path = "../local" }
git_package = {
    git = "https://example.com/team/package.git",
    rev = "0123456789abcdef0123456789abcdef01234567"
}
json = { package = "flecha/json", version = "^1.2.0" }
```

Path dependencies support local development. Git dependencies bridge the
period before a package is published and are pinned to an exact commit in the
lockfile. Registry dependencies use semantic version requirements and resolve
to an immutable published version.

A publishable dependency may combine a registry identity and version with a
development path override. Publishing records the registry requirement and
never embeds the local path.

## Package graph

The current global `--lib name=path` map cannot represent recursive package
dependencies. Two packages may use the same alias for different dependencies,
so dependency aliases must be interpreted relative to the importing package.

Longbow therefore produces a package graph. Each node records:

- its canonical package identity and exact version or revision;
- its source root and content checksum;
- its compatible Flecha version range; and
- a map from that package's dependency aliases to other graph nodes.

The compiler resolves `lib::<alias>` through the map belonging to the source
identity of the importing unit. It must not flatten aliases across the graph.
`root` continues to resolve inside the importing package's own source root.

The 0.3 resolver selects one exact version of each canonical owner/package
identity for the complete graph. Conflicting requirements are diagnosed rather
than resolved as parallel versions. Node identity and the versioned graph
schema must not assume a global alias map, leaving parallel-version resolution
as a compatible future extension.

The graph is a generated compiler input under `target`; it is not a handwritten
public configuration format. Its schema is versioned so Longbow and Flecha can
diagnose incompatible toolchain components.

Schema 1 is JSON:

```json
{
  "schema": 1,
  "root": "workspace",
  "packages": [
    {
      "id": "workspace",
      "identity": "local/application",
      "source": "../src",
      "dependencies": {
        "json": "flecha-json@1.2.3",
        "std": "toolchain-std"
      }
    },
    {
      "id": "flecha-json@1.2.3",
      "identity": "flecha/json",
      "version": "1.2.3",
      "source": "/verified/cache/source",
      "checksum": "sha256:...",
      "flecha": ">=0.3.0, <0.4.0",
      "dependencies": {}
    },
    {
      "id": "toolchain-std",
      "identity": "flecha/std",
      "source": "/toolchain/share/flecha/std/src",
      "dependencies": {}
    }
  ]
}
```

`root` names the node containing the compiler entry source. Node `id` values
are opaque, graph-unique compilation identities rather than import aliases or
canonical registry names. This keeps module, nominal-type, error-family, and
generic-specialization identities isolated even when packages contain the same
module and declaration names. Each `dependencies` key is a local Flecha import
alias and its value is a target node id. `source` is absolute or relative to
the graph file. `version`, `revision`, `checksum`, and `flecha` carry resolved
source metadata when applicable.

The compiler accepts the graph through `--package-graph`, rejects unsupported
schemas, missing nodes, invalid aliases, dependency cycles, and entries outside
the root node's source tree, and resolves every import without network access.
`--package-graph` is mutually exclusive with the compatibility `--root` and
`--lib` options.

## Lockfile and cache

`longbow.lock` records the complete selected graph, including exact versions,
Git revisions, sources, dependency edges, and archive checksums. Applications
commit it. Libraries may commit it for reproducible development and tests, but
their consumers resolve from published version requirements rather than the
library's development lock.

The lockfile is authoritative after resolution. A package whose downloaded or
cached contents do not match its checksum is rejected rather than silently
accepted.

Longbow uses the platform's native user-cache location and supports an explicit
environment override for CI and managed installations. Cache entries are
content-addressed, shared across projects, and safe to remove. A verified cache
must support offline builds without contacting the registry or source origin.

## Publication

`longbow publish --dry-run` creates the normalized archive, validates it, and
prints its included files and checksum without uploading. `longbow publish`
uploads that exact archive after authenticating an owner or maintainer.

The initial archive contains source and package documentation such as:

```text
longbow.toml
src/**/*.fl
README.md
LICENSE*
```

It excludes version-control data, `target`, local lock state, editor state,
temporary files, and secrets. Archive validation rejects absolute paths,
parent traversal, symbolic links, case-colliding paths, excessive sizes, and
unsupported file types.

Published owner/package/version tuples are immutable. A maintainer may yank a
version so new resolutions avoid it, but an existing lock can continue using
and downloading the archived content. Deleting or replacing published bytes is
not an ordinary maintainer operation.

The manifest's `package.registry` field declares the canonical
`owner/package` publication identity and is retained in every snapshot.
Longbow serializes archives deterministically, removes development path
overrides from the normalized manifest, sorts paths bytewise, and hashes the
exact bytes with SHA-256. Extraction accepts only `longbow.toml`, Flecha files
below `src`, `README.md`, and root `LICENSE*` documents. Traversal, symbolic
links, case collisions, excessive sizes, trailing data, native inputs, and
other file types are rejected.

Public packages are pure Flecha source in 0.3.0. Precompiled objects, native
archives, build scripts, and target-specific package payloads require separate
ABI, security, licensing, and reproducibility designs and are not accepted by
the first registry.

## Longbow commands

The initial user workflow is:

```text
longbow add OWNER/PACKAGE@REQUIREMENT [--as ALIAS]
longbow remove ALIAS
longbow fetch
longbow update [ALIAS]
longbow login
longbow whoami
longbow logout
longbow publish --dry-run
longbow publish
longbow yank VERSION
```

`latest` is accepted as a command-line convenience for `add` and is normalized
to the stable-version `*` requirement in `longbow.toml`. Resolution still
records one exact immutable version and checksum in `longbow.lock`.

`longbow login` starts the registry's device authorization flow. The developer
signs in or registers with GitHub or Google in a browser, approves the displayed
code, and receives a separate revocable Flecha registry token. Provider access
tokens are never used as package credentials. Owner membership is evaluated by
the registry on each mutation, so claiming an owner or changing a role takes
effect without issuing another CLI token. CI may continue supplying
`LONGBOW_TOKEN` directly.

Resolution and fetch are separate internally even when `build`, `check`,
`test`, or `run` invokes both. Diagnostics identify whether failure came from
manifest validation, version selection, network acquisition, checksum
verification, cache extraction, or compilation.

## Registry protocol v1

Longbow uses an HTTPS base of `https://registry.flecha-lang.org/v1`. Package
path components use the already-validated lowercase owner and package names.
The initial client contract is:

| Request | Meaning |
| --- | --- |
| `GET /packages/OWNER/PACKAGE/index` | Read immutable releases and yank state |
| `GET /packages/OWNER/PACKAGE/VERSION/archive` | Download exact normalized archive bytes |
| `PUT /packages/OWNER/PACKAGE/VERSION/archive` | Publish once, with bearer authorization and `X-Flecha-Checksum` |
| `POST /packages/OWNER/PACKAGE/VERSION/yank` | Mark the immutable release unavailable to new selections |

The versioned index media is UTF-8 text. Its first line is
`flecha-registry-index 1`; each following line is
`VERSION<TAB>sha256:HEX<TAB>active|yanked`. Duplicate or malformed versions,
unknown states, and invalid checksums are rejected. Archives use
`application/vnd.flecha.package.v1`. Clients verify the checksum before
extraction and verify the normalized extracted tree again on every cache use.
Yanking changes only index state: an existing lock may still fetch and build
the recorded checksum, while an unlocked resolution skips that release.

This repository implements the protocol client and a filesystem transport for
deterministic integration tests; deployment of the public service is separate
backend work.

## Registry ownership and trust

The registry supports user and organization owners, multiple package
maintainers, scoped publishing tokens, ownership audit history, and a reserved
official `flecha` owner. Initial login may use GitHub identity, but registry
ownership is stored by Flecha rather than inferred permanently from a mutable
repository URL.

Later trusted publishing may exchange GitHub Actions or another CI identity for
a short-lived package-scoped credential. That does not change package identity,
archive immutability, or checksum verification.

Private registries, private packages, registry federation, mirrors, native
package artifacts, and precompiled Flecha packages are outside the 0.3.0
contract.
