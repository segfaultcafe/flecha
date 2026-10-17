# Contributing to Flecha

Thank you for helping build Flecha. The project welcomes bug reports,
documentation improvements, design discussions, tests, and focused pull
requests. Flecha is still young, so clear reasoning and maintainable code are
more valuable than the size or speed of a contribution.

## Human authorship and AI-assisted contributions

Flecha is intended to become a language that people can understand, maintain,
and eventually use to implement more of its own toolchain. Human authorship is
therefore an explicit project value.

AI agents and generative coding tools may author implementation changes only
for core compiler and toolchain features written in C++. This includes the C++
front end, semantic analysis, lowering, LLVM code generation, native compiler,
and directly related C++ tests.

AI agents must not author:

- Flecha standard-library code, including files under `std/`;
- features implemented in Flecha source (`.fl` files);
- any future self-hosted compiler, toolchain, or runtime code;
- non-core product features, examples, or editor integrations; or
- a mixed contribution in which generated C++ is used as a route to generate
  or substantially design code in one of the areas above.

Those contributions must be written by a human. Using ordinary, deterministic
tools such as compilers, formatters, linters, and search-and-replace is not
considered AI authorship.

When an AI agent was used in an allowed C++ contribution, disclose it clearly
in the pull-request description. State which parts were agent-authored or
agent-assisted. The human contributor must understand and take responsibility
for every submitted line, verify its provenance and licensing, and be able to
explain or revise it during review. Prefer separating human-authored Flecha
work and AI-assisted C++ work into different pull requests.

Maintainers may ask for a contribution to be rewritten or may close it when
its authorship is unclear or this policy is not followed. When in doubt, open
an issue before writing code.

## Required fork workflow

External contributors must work in a personal fork. Pull requests from
non-maintainers whose source branch belongs to the upstream Flecha repository
are not accepted.

1. Fork `alexbsec/flecha` on GitHub.
2. Clone your fork and add the upstream repository as a read-only remote.
3. Create a focused branch from the appropriate base branch in your fork.
4. Push the branch to your fork.
5. Open a pull request into `master` for planned development or `release` for
   an approved patch to the currently released version.

GitHub cannot prevent someone from making a read-only clone of a public
repository. The project therefore enforces the part that affects repository
integrity: contributors do not receive a direct upstream branch and CI rejects
non-maintainer pull requests sourced from one. Only maintainers may work
directly against the upstream repository. At present, the repository owner is
the only person authorized to do so.

## Development and release branches

Flecha keeps planned development separate from the currently released line:

- `master` contains work for the next planned minor or major release. It may
  include compatible features and other changes that are not part of the
  current release series. It is the default integration branch and the base
  for features, ordinary fixes, refactors, and documentation intended for the
  next version.
- `release` represents the latest shipped production line. Immediately after
  a release is completed, its head must be the commit carrying the latest
  version tag. It accepts only approved patches to the current version and
  promotion pull requests from `master`; feature development does not happen
  directly on this branch.

Ordinary commits on either branch are not releases and must not receive a
version tag. Releases are deliberate milestones following semantic versioning:
compatible feature work accumulates toward a minor version, incompatible work
toward a major version, and only necessary fixes to an already shipped version
justify a patch release.

Direct pushes to `master` and `release` are not part of the normal workflow.
Maintainers push focused branches to the upstream repository and open ordinary
pull requests. Non-maintainers push branches to personal forks, open pull
requests from those forks, and require at least one maintainer approval. Both
types of pull request must pass the required checks and resolve review threads.

Use the following push flow for planned development:

1. Start a focused `feat/<name>`, `fix/<name>`, or documentation branch from
   the latest `master`.
2. Push it to the upstream repository when working as a maintainer, or to a
   personal fork otherwise.
3. Open a pull request against `master`.
4. Merge only after the applicable review and required checks pass.

Commits on `master` are integration commits, not releases, and are never tagged
as versions.

Use the following workflow for an urgent patch:

1. Confirm that the change must ship before the next planned minor or major
   release.
2. Create `bugfix/<name>` from `release`, not from `master`.
3. Open the focused pull request against `release` and run the complete release
   checks.
4. Merge the fix, finish the patch release metadata, and tag the resulting
   `release` commit (for example, `v0.1.2`).
5. Immediately merge `release` forward into `master`, resolving conflicts in
   favor of retaining both the patch and the newer development work.

When the planned minor or major version is ready, ensure `master` contains all
patches, merge `master` into `release`, complete the release checks and
metadata, and tag that `release` commit. New development then continues on
`master`. This keeps every shipped fix in future versions without forcing a
tag for each development commit.

Every version tag must point to the exact current head of `release`. Version
tags use the form `vMAJOR.MINOR.PATCH`, are created only by a maintainer after
the release pull request and checks complete, and must never point directly to
`master` or to an older `release` commit. The release workflow verifies this
identity before publishing artifacts.

## Review and merge policy

Every pull request from a contributor fork requires at least one approving
review from a Flecha maintainer before it can be merged. The author cannot
approve their own work. New commits that materially change the reviewed diff
may require a fresh approval, and all required CI checks must still pass.
GitHub enforces every Linux test, sanitizer, editor, and cross-platform package
check on protected branches. A failed, canceled, skipped, or missing required
check blocks the merge, and the pull-request branch must be up to date with its
target branch.

Maintainers may work directly in the upstream repository. For now, a
maintainer's own pull request does not require a second maintainer's approval,
although it remains subject to CI and the same authorship, testing, and quality
rules. This exception may be tightened as the maintainer group grows.

## Becoming a maintainer

Maintainers are trusted stewards rather than simply frequent contributors.
Maintainer access is granted by invitation from the repository owner; meeting
the prerequisites makes someone eligible but does not grant access
automatically.

A prospective maintainer should have:

- a sustained record of several meaningful, merged contributions across more
  than one change, with regression tests and documentation where appropriate;
- demonstrated understanding of at least one compiler subsystem and a working
  understanding of how parsing, semantics, lowering, code generation, the
  runtime, and the standard library fit together;
- participated constructively in reviews, issue triage, or design discussions;
- consistently followed the human-authorship, fork, licensing, and disclosure
  policies;
- shown sound judgment about compatibility, diagnostics, security, and when to
  ask for broader design agreement;
- a respectful and reliable record in the project community; and
- two-factor authentication enabled on their GitHub account.

Maintainers are expected to review contributor pull requests carefully,
protect the human-authored areas of the project, keep CI and releases healthy,
and disclose conflicts of interest. Access may be removed after prolonged
inactivity, misuse, repeated policy violations, or at the maintainer's request.

## Before you begin

For a bug fix, search the issue tracker and include a minimal reproduction.
For a language or library change, start with a design discussion. Describe:

- the problem and intended behavior;
- syntax and type-system effects;
- diagnostics and failure behavior;
- lowering, code-generation, and runtime implications;
- compatibility or migration concerns; and
- alternatives considered.

The documents in `docs/` and `SPEC.md` include both implemented behavior and
proposals. Compiler tests and diagnostics are the authority when the two do
not yet agree.

## Building and testing

The main requirements are CMake 3.20 or newer, Ninja (or another CMake
generator), a C++23 compiler, and the LLVM development libraries and tools.
Build the compiler and Longbow with:

```sh
make build
```

Run the complete test suite with:

```sh
make test
```

Useful focused checks include:

```sh
make test-semantic
make test-neovim
make test-sanitizers
```

Release-related changes should also produce and inspect the distributable
archives:

```sh
cmake --build build --target package
```

See `README.md` for setup details and additional native smoke-test targets.

## Pull-request expectations

Keep pull requests focused and make their intent easy to review. A good pull
request:

- originates from a branch in the contributor's fork, unless its author is a
  maintainer;
- explains the problem, the chosen approach, and important tradeoffs;
- links the relevant issue or design discussion;
- includes regression tests for changed behavior;
- preserves source ranges and stable diagnostic identifiers where practical;
- updates `SPEC.md`, design documents, and examples when behavior changes;
- follows `CODE_STYLE.md` for C++ changes;
- avoids committing build outputs, credentials, or unrelated formatting; and
- reports the commands used to validate the change.

Changes to the language should normally cover the full path through parsing,
semantic analysis, lowering, code generation or runtime behavior, diagnostics,
and tests. If a pull request intentionally implements only part of that path,
state the limitation explicitly.

All continuous-integration checks must pass. Platform-specific changes should
be tested on every affected operating system when possible; the repository CI
builds and exercises Linux, Windows, and macOS packages.

## Reporting bugs

Please include:

- the Flecha commit or release version;
- operating system, architecture, C++ compiler, and LLVM version;
- the smallest source file or command that reproduces the problem;
- expected and actual behavior; and
- complete diagnostics or a backtrace, if available.

Remove secrets and private paths from logs before posting them.

## Review and conduct

Reviews may ask for simpler code, stronger tests, a clearer design, or a
smaller change. Treat review as collaboration: discuss the reasoning, update
the change when appropriate, and keep follow-up commits understandable.

Be respectful and patient with contributors of every experience level. Focus
technical discussion on the work, assume good intent, and make room for people
to learn without lowering the project's correctness standards.

By contributing, you agree that your work may be distributed under the terms
of the repository's `LICENSE`.
