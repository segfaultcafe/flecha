"""Conservative local CTest selection; full CI remains the integration gate.

This is an explicit policy, not a claim of complete dependency analysis. Any
path without a supported mapping selects the full suite. Inputs and reasons are
kept separate from execution so callers can display and audit their selection.
"""

from __future__ import annotations

import os
from pathlib import Path, PurePosixPath
import re
import subprocess


class SelectionError(RuntimeError):
    """Changes could not be determined reliably; callers must not skip tests."""


def changed_files(repo: Path, base: str = "master") -> list[str]:
    """Return branch, staged, unstaged, and untracked changes relative to base.

    Merge-base preserves branch changes when master advances. Disabling rename
    detection preserves both paths, including the subsystem a file left.
    Git failures raise instead of looking like an unchanged working tree.
    """
    def git(*arguments: str) -> bytes:
        try:
            result = subprocess.run(
                ["git", "-C", str(repo), *arguments],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
            )
        except (OSError, subprocess.CalledProcessError) as error:
            detail = getattr(error, "stderr", b"")
            raise SelectionError(
                "Cannot determine changed files: "
                + (os.fsdecode(detail).strip() or str(error))
            ) from error
        return result.stdout

    ancestor = os.fsdecode(git("merge-base", base, "HEAD")).strip()
    if not ancestor:
        raise SelectionError("Cannot determine changed files: empty merge-base")
    changes = (
        git("diff", "--name-only", "--no-renames", "-z", ancestor, "HEAD", "--"),
        git("diff", "--name-only", "--no-renames", "-z", "HEAD", "--"),
        git("ls-files", "--others", "--exclude-standard", "-z"),
    )
    return sorted({os.fsdecode(path) for data in changes for path in data.split(b"\0") if path})


def _command_values(test: dict) -> list[str]:
    # CTest's JSON command is already an argument vector. Never shell-split it.
    return [str(arg).split("=", 1)[-1] for arg in test.get("command", [])]


def select_tests(
    repo: Path, tests: list[dict], changed_paths: list[str]
) -> tuple[list[dict], list[str]]:
    """Select the union of affected tests, preserving discovery order.

    A focused rule that matches no tests also falls back to the full suite;
    renamed/removed tests or incomplete discovery must not silently skip work.
    An empty change set returns an empty selection, with an explicit reason.
    """
    if not changed_paths:
        return [], ["No changed files; use full mode to validate the integration boundary."]

    selected: set[str] = set()
    reasons: list[str] = []
    names = {test["name"] for test in tests}

    def full(reason: str) -> tuple[list[dict], list[str]]:
        return list(tests), [*reasons, "Full suite: " + reason]

    for path in sorted(set(changed_paths)):
        relative = PurePosixPath(path)
        if relative.is_absolute() or ".." in relative.parts:
            return full(f"unsupported repository path {path!r}.")
        path = relative.as_posix()
        matches: set[str]
        rule: str

        # Infrastructure and shared language/runtime changes are deliberately
        # broad. In particular, std imports cross module boundaries and the
        # current standard-library CTest aggregates all logical modules.
        if relative.name == "CMakeLists.txt" or path.startswith((
            "cmake/", ".github/", "std/", "tools/", "src/tests/fixtures/",
        )) or path in {"Makefile", "CMakePresets.json", "CMakeUserPresets.json"}:
            return full(f"{path} changes shared code, fixtures, or test/build infrastructure.")

        if path.startswith("src/tests/test_") and relative.suffix == ".cpp":
            source = repo / path
            if relative.name == "test_main.cpp" or not source.is_file():
                return full(f"{path} is shared test setup or a removed test source.")
            try:
                contents = source.read_text(encoding="utf-8")
            except (OSError, UnicodeError):
                return full(f"cannot read test suites from {path}.")
            suites = set(re.findall(r"\bTEST(?:_F|_P)?\s*\(\s*(\w+)\s*,", contents))
            if not suites:
                return full(f"no supported test declarations found in {path}.")
            # Ordinary, fixture, and parameterized GTest CTest names.
            patterns = [re.compile(r"(?:^|/)" + re.escape(suite) + r"(?:/[^.]*)?\.") for suite in suites]
            matches = {name for name in names if any(pattern.search(name) for pattern in patterns)}
            # Every parsed suite must be represented in discovery.
            if any(not any(pattern.search(name) for name in names) for pattern in patterns):
                return full(f"test suites from {path} are missing in discovery.")
            rule = "C++ test suites"
        elif path.startswith("src/tests/verify_") and relative.suffix == ".cmake":
            absolute = str((repo / path).resolve())
            matches = {test["name"] for test in tests if absolute in _command_values(test)}
            rule = "verification helper command"
        elif path.startswith(("src/analyzer/", "src/include/analyzer/", "src/lsp/", "src/include/lsp/")):
            if relative.suffix not in {".cpp", ".hpp", ".h"}:
                return full(f"unsupported analyzer/LSP input {path}.")
            matches = {name for name in names if name.startswith((
                "Analyzer", "AnalysisSession", "SourceOverlays", "SourceProvider", "Lsp",
            )) or name in {"CliTests.AnalyzerStdio", "CliTests.LspStdio"}}
            rule = "analyzer and LSP dependents"
        elif path.startswith(("src/longbow/", "src/include/longbow/")):
            if relative.suffix not in {".cpp", ".hpp", ".h"}:
                return full(f"unsupported Longbow input {path}.")
            matches = {test["name"] for test in tests if test["name"].startswith((
                "Longbow", "LanguageTestTests.",
            )) or test["name"] in {"HardeningCampaign.Fast", "StandardLibraryTests.FlechaModules", "CliTests.LongbowVersion"}
                or any(Path(value).name in {"longbow", "longbow.exe"} for value in _command_values(test))}
            rule = "Longbow unit tests and command dependents"
        elif path.startswith("editors/vscode/"):
            matches = names & {"EditorTests.VSCodeManifest"}
            rule = "VS Code CTest checks (npm test/compile remain separate)"
        elif (path.startswith("docs/") and relative.suffix == ".md") or path in {
            "README.md", "CONTRIBUTING.md", "CODE_STYLE.md", "SPEC.md", "CHANGELOG.md",
        }:
            # These verification scripts read documentation as test inputs.
            # Keep their assertions in the focused path alongside the example
            # smoke check; omitting an unavailable consumer is not safe.
            consumers = {
                "docs/editors.md": {"EditorTests.VSCodeManifest"},
                "docs/toolchains.md": {"BuildDiagnostics.ToolchainConsistency"},
                "README.md": {"BuildDiagnostics.ToolchainConsistency"},
            }
            matches = {"DocumentationExampleTests.CheckBuildRun"} | consumers.get(path, set())
            if matches - names:
                return full(f"documentation consumers for {path} are missing in discovery: "
                            + ", ".join(sorted(matches - names)) + ".")
            rule = "documented example smoke check and direct documentation consumers"
        else:
            return full(f"{path} has no conservative focused mapping.")

        if not matches:
            return full(f"{path} mapped to {rule}, but no matching tests were discovered.")
        selected.update(matches)
        reasons.append(f"{path}: {rule} ({len(matches)} tests).")

    return [test for test in tests if test["name"] in selected], reasons
