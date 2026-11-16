"""Conservative, local successful-result cache for the CTest runner.

Only the reviewed, in-memory GoogleTest suites below opt in. Adding a suite is
a dependency/purity review: no runtime source/fixture/file reads, subprocesses,
network, clocks, randomness, or external state. Integration tests never opt in.
The runner must complete a successful incremental build before looking up keys.
Callers must hold their build/run lock throughout discovery, snapshots, running,
and writes. This does not coordinate direct invocations of Ninja or CTest.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import tempfile
from dataclasses import dataclass
from typing import Mapping


CACHE_VERSION = 1
# Reviewed in test_ast/parser/tokenizer/method_key/diagnostics/semantic/lowering/
# async_plan.cpp. New tests in these suites must preserve this purity contract.
PURE_SUITES = frozenset("""
AstTests AsyncPlanTests DiagnosticTests LoweringTests MethodContractTests
MethodKeyTests ScopeTests TokenizerTests
ParserAllocationTests ParserArrayTests ParserAssignmentTests ParserCallTests
ParserCallableTests ParserCastTests ParserConditionalTests ParserDeferTests
ParserEnumTests ParserErrorTests ParserExpressionTests ParserInterfaceTests
ParserLoopControlTests ParserSliceTests ParserStringTests ParserStringViewTests
ParserStructureTests ParserTests ParserTupleTests ParserUnsafeTests
ParserVariableDeclarationTests ParserWhileTests
SemanticAllocationTests SemanticAnalyzerTests SemanticArrayTests
SemanticBuiltInTests SemanticCallTests SemanticCallableTests SemanticCastTests
SemanticCompoundTypeTests SemanticConstTests SemanticControlFlowAssignmentTests
SemanticConversionTests SemanticDeclarationTests SemanticDecorationTests
SemanticDeferTests SemanticDiagnosticTests SemanticEnumTests SemanticErrorTests
SemanticExternTests SemanticGenericTests SemanticInferenceTests
SemanticIntegrationTests SemanticInterfaceCallTests SemanticInterfaceConversionTests
SemanticInterfaceMethodTests SemanticInterfaceProviderCallTests SemanticInterfaceTests
SemanticLoopControlTests SemanticMatchTests SemanticMemoryTests SemanticOperatorTests
SemanticOverloadTests SemanticReferenceTests SemanticScopeTests SemanticSliceTests
SemanticStringTests SemanticStringViewTests SemanticStructureTests SemanticTests
SemanticTupleTests SemanticTypeTests SemanticUnsafeTests
""".split())


def _json(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True).encode()


def _record_digest(test: dict) -> str:
    return hashlib.sha256(_json(test)).hexdigest()


def _execution_inputs(test: dict) -> tuple[set[Path], set[Path]] | None:
    """Recognize direct GoogleTest and CMake 4.3's exact standard launcher."""
    name, command = test.get("name", ""), test.get("command", [])
    if not isinstance(command, list) or not all(isinstance(arg, str) for arg in command):
        return None
    if len(command) in (2, 3) and Path(command[0]).name == "test_flecha":
        if command[1] == "--gtest_filter=" + name and (
                len(command) == 2 or command[2] == "--gtest_also_run_disabled_tests"):
            return {Path(command[0])}, set()
    if (len(command) == 13 and Path(command[0]).name == "cmake"
            and command[1:11:2] == ["-D"] * 5 and command[11] == "-P"
            and command[2].startswith("TEST_EXECUTABLE=")
            and command[4] == "TEST_EXECUTOR="
            and command[6] == "TEST_FILTER=" + name
            and command[8] == "TEST_XML_OUTPUT="
            and command[10] == "TEST_EXTRA_ARGS="):
        binary = Path(command[2].split("=", 1)[1])
        script = Path(command[12])
        if (binary.name == "test_flecha" and binary.is_absolute()
                and script.is_absolute() and script.parts[-2:] == ("GoogleTest", "LaunchTest.cmake")):
            return {Path(command[0]), binary}, {script}
    return None


def eligible_test(test: dict) -> bool:
    """Require an exact, single GoogleTest invocation and no CTest fixtures."""
    name = test.get("name", "")
    if not isinstance(name, str) or name.split(".", 1)[0] not in PURE_SUITES:
        return False
    if _execution_inputs(test) is None:
        return False
    if any(character in name for character in "*?:-"):
        return False
    # Fail closed on additional execution semantics, especially fixture setup /
    # cleanup: a cached setup must never suppress artifacts needed by a test.
    allowed_properties = {"WORKING_DIRECTORY", "SKIP_REGULAR_EXPRESSION",
                          "LABELS", "TIMEOUT", "PROCESSORS", "RESOURCE_LOCK",
                          "COST", "DEF_SOURCE_LINE"}
    return all(prop.get("name") in allowed_properties
               for prop in test.get("properties", []))


@dataclass(frozen=True)
class Snapshot:
    digest: str
    test_keys: Mapping[str, str]

    def key(self, test: dict) -> str | None:
        return self.test_keys.get(_record_digest(test))


class CacheUnavailable(Exception):
    pass


class CacheContext:
    def __init__(self, source_dir: Path, build_dir: Path,
                 environment: Mapping[str, str] | None = None,
                 ctest: str = "ctest"):
        self.source_dir = Path(source_dir).resolve()
        self.build_dir = Path(build_dir).resolve()
        self.environment = dict(os.environ if environment is None else environment)
        self.ctest = ctest
        self.path = self.build_dir / ".flecha-test-cache.json"
        self.disabled_reason: str | None = None
        self._successes: set[str] | None = None

    def _run(self, args: list[str], *, environment: dict | None = None) -> bytes:
        try:
            result = subprocess.run(args, cwd=self.source_dir,
                                    env=self.environment if environment is None else environment,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=30, check=True)
            return result.stdout
        except (OSError, subprocess.SubprocessError) as error:
            # Never expose command output/environment, which may contain secrets.
            raise CacheUnavailable(f"dependency inspection failed ({type(error).__name__})") from error

    def _source_files(self) -> list[Path]:
        git_environment = {k: v for k, v in self.environment.items()
                           if not k.startswith("GIT_")}
        root = self._run(["git", "rev-parse", "--show-toplevel"],
                         environment=git_environment).decode().strip()
        if Path(root).resolve() != self.source_dir:
            raise CacheUnavailable("source directory is not the Git repository root")
        names = self._run(["git", "ls-files", "-z", "--cached", "--others",
                           "--exclude-standard"], environment=git_environment)
        paths = []

        def collect(directory: Path, entries: bytes) -> None:
            for name in sorted(set(entries.split(b"\0")) - {b""}):
                path = directory / os.fsdecode(name)
                # A custom, nonignored build directory must not hash its own
                # cache, logs or mutable test output.
                if path == self.build_dir or self.build_dir in path.parents:
                    continue
                if path.is_dir() and not path.is_symlink():
                    if not any(path.iterdir()):
                        # Optional, unused submodules may be uninitialized while
                        # CMake uses a system package. Preserve their emptiness in
                        # the snapshot; later initialization changes the key.
                        paths.append(path)
                        continue
                    # git ls-files returns submodules as directories. Enumerate
                    # their actual working trees, including dirty/untracked
                    # files; a gitlink commit alone misses local dependency edits.
                    subroot = self._run(["git", "-C", str(path), "rev-parse", "--show-toplevel"],
                                        environment=git_environment).decode().strip()
                    if Path(subroot).resolve() != path.resolve():
                        raise CacheUnavailable("submodule is not initialized")
                    children = self._run(["git", "-C", str(path), "ls-files", "-z",
                                          "--cached", "--others", "--exclude-standard"],
                                         environment=git_environment)
                    collect(path, children)
                else:
                    paths.append(path)

        collect(self.source_dir, names)
        return sorted(paths)

    def _shared_libraries(self, executable: Path) -> list[Path]:
        output = self._run(["ldd", str(executable)]).decode(errors="replace")
        libraries = []
        for line in output.splitlines():
            line = line.strip()
            if not line or line.startswith("linux-vdso.so"):
                continue
            match = re.fullmatch(r"(?:\S+ => )?(/.+?) \(0x[0-9a-fA-F]+\)", line)
            if not match:
                raise CacheUnavailable("could not resolve every runtime shared library")
            libraries.append(Path(match.group(1)))
        if not libraries:
            raise CacheUnavailable("runtime shared-library inventory is empty")
        return libraries

    def _test_definitions(self) -> list[Path]:
        definitions = []
        for path in self.build_dir.rglob("*.cmake"):
            name = path.name.lower()
            if name.startswith("ctest") or ("test" in name and name.endswith(
                    ("_include.cmake", "_tests.cmake"))):
                definitions.append(path)
        return sorted(definitions)

    @staticmethod
    def _process_context() -> dict:
        # Explicit resource constraints can make an otherwise identical pure
        # test fail. They are not represented by environment variables.
        import resource
        limits = {name: resource.getrlimit(getattr(resource, name))
                  for name in dir(resource) if name.startswith("RLIMIT_")}
        return {"limits": limits, "uid": os.geteuid(), "gid": os.getegid(),
                "groups": sorted(os.getgroups())}

    @staticmethod
    def _hash_path(path: Path, *, missing_ok: bool = False) -> str:
        digest = hashlib.sha256()
        digest.update(os.fsencode(str(path)))
        try:
            before = path.stat()
            if stat.S_ISDIR(before.st_mode) and not path.is_symlink():
                if any(path.iterdir()):
                    raise CacheUnavailable("dependency directory changed while hashing")
                digest.update(b"<empty directory>")
                return digest.hexdigest()
            if not stat.S_ISREG(before.st_mode):
                raise CacheUnavailable("dependency is not a regular file")
            digest.update(str(stat.S_IMODE(before.st_mode)).encode())
            digest.update(f"{before.st_uid}:{before.st_gid}".encode())
            digest.update(os.fsencode(str(path.resolve())))
            with path.open("rb") as handle:
                for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                    digest.update(chunk)
            after = path.stat()
            if (before.st_ino, before.st_size, before.st_mtime_ns,
                    before.st_ctime_ns) != (after.st_ino, after.st_size,
                                          after.st_mtime_ns, after.st_ctime_ns):
                raise CacheUnavailable("dependency changed while hashing")
        except FileNotFoundError:
            if not missing_ok or path.is_symlink():
                raise CacheUnavailable("required dependency is missing")
            digest.update(b"<deleted source>")
        except OSError as error:
            raise CacheUnavailable("dependency cannot be read") from error
        return digest.hexdigest()

    def snapshot(self, tests: list[dict]) -> Snapshot | None:
        """Hash inputs after building; pass the complete CTest inventory each time.

        An unsupported or incomplete dependency inventory disables reuse, never
        silently omits dependencies. Only hashes, not environment values, persist.
        """
        self.disabled_reason = None
        try:
            if platform.system() != "Linux":
                raise CacheUnavailable("result caching currently requires Linux")
            if self.source_dir == self.build_dir or self.build_dir in self.source_dir.parents:
                raise CacheUnavailable("result caching requires a separate build directory")
            if any(key.startswith("GTEST_") or key in {
                    "LD_PRELOAD", "LD_AUDIT", "ASAN_OPTIONS", "LSAN_OPTIONS",
                    "UBSAN_OPTIONS", "TSAN_OPTIONS", "MSAN_OPTIONS", "HWASAN_OPTIONS"}
                   for key in self.environment):
                raise CacheUnavailable("GoogleTest, loader, or sanitizer overrides are active")
            selected = [test for test in tests if eligible_test(test)]
            binaries: set[Path] = set()
            scripts: set[Path] = set()
            for test in selected:
                execution = _execution_inputs(test)
                assert execution is not None
                binaries.update(execution[0])
                scripts.update(execution[1])
            if any(not binary.is_absolute() for binary in binaries):
                raise CacheUnavailable("test executable path is not absolute")
            ctest_path = shutil.which(self.ctest, path=self.environment.get("PATH", os.defpath))
            if not ctest_path:
                raise CacheUnavailable("CTest executable cannot be resolved")
            binaries.add(Path(ctest_path).resolve())
            required = {self.build_dir / "CMakeCache.txt",
                        self.build_dir / "compile_commands.json", Path(__file__).resolve(),
                        Path(__file__).resolve().with_name("test_runner.py"),
                        Path(__file__).resolve().with_name("test_progress.py"),
                        Path(__file__).resolve().with_name("test_selection.py")}
            required.update(scripts)
            required.update(self._test_definitions())
            cache_text = (self.build_dir / "CMakeCache.txt").read_text()
            compiler_paths = re.findall(r"^CMAKE_(?:C|CXX)_COMPILER:[^=]+=(.+)$",
                                        cache_text, re.MULTILINE)
            if len(compiler_paths) != 2:
                raise CacheUnavailable("both configured compiler paths are required")
            for compiler in compiler_paths:
                resolved = shutil.which(compiler, path=self.environment.get("PATH", os.defpath))
                if not resolved:
                    raise CacheUnavailable("configured compiler cannot be resolved")
                required.add(Path(resolved))
            for binary in binaries:
                required.add(binary)
                required.update(self._shared_libraries(binary))
            # Loader configuration influences resolution even when executable and
            # shared-library contents are unchanged.
            for configuration in ("/etc/ld.so.cache", "/etc/ld.so.preload"):
                if Path(configuration).exists():
                    required.add(Path(configuration))
            source_files = self._source_files()
            execution = {
                "version": CACHE_VERSION,
                "platform": list(platform.uname()),
                "process": self._process_context(),
                "environment": self.environment,
                "dependencies": [self._hash_path(path) for path in sorted(required)],
            }
            execution_digest = hashlib.sha256(_json(execution)).hexdigest()
            # Reviewed pure tests consume only their executable/runtime inputs.
            # The runner ALWAYS builds first, so unchanged binaries can reuse
            # validation across documentation or unrelated source edits. Source
            # contents still guard against changes during the active run.
            guard = {"execution": execution_digest, "tests": tests,
                     "source": [self._hash_path(path, missing_ok=True) for path in source_files]}
            digest = hashlib.sha256(_json(guard)).hexdigest()
            keys = {_record_digest(test): hashlib.sha256(
                (execution_digest + _record_digest(test)).encode()).hexdigest() for test in selected}
            return Snapshot(digest, keys)
        except (CacheUnavailable, OSError, UnicodeError, ImportError) as error:
            self.disabled_reason = (str(error) if isinstance(error, CacheUnavailable)
                                    else "dependency inventory could not be read")
            return None

    def _load(self) -> set[str]:
        if self._successes is None:
            self._successes = set()
            try:
                data = json.loads(self.path.read_text())
                if data.get("version") == CACHE_VERSION and isinstance(data.get("successes"), list):
                    self._successes = {key for key in data["successes"]
                                       if isinstance(key, str) and re.fullmatch(r"[0-9a-f]{64}", key)}
            except (OSError, ValueError, AttributeError):
                pass
        return self._successes

    def lookup(self, test: dict, snapshot: Snapshot | None) -> bool:
        key = snapshot.key(test) if snapshot else None
        return key is not None and key in self._load()

    def _save(self) -> None:
        self.build_dir.mkdir(parents=True, exist_ok=True)
        descriptor, temporary = tempfile.mkstemp(prefix=".test-cache-", dir=self.build_dir)
        try:
            with os.fdopen(descriptor, "w") as handle:
                json.dump({"version": CACHE_VERSION, "successes": sorted(self._load())}, handle)
                handle.write("\n")
            os.replace(temporary, self.path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)

    def invalidate(self, tests: list[dict], snapshot: Snapshot | None) -> None:
        """Forget previous success before a forced rerun, including failed refreshes."""
        if snapshot is None:
            return
        successes = self._load()
        old_count = len(successes)
        for test in tests:
            successes.discard(snapshot.key(test))
        if len(successes) != old_count:
            self._save()

    def record_successes(self, tests: list[dict], snapshot: Snapshot | None,
                         after_snapshot: Snapshot | None) -> int:
        """Caller supplies only actual passes, excluding skipped/not-run tests."""
        if snapshot is None or after_snapshot is None or snapshot.digest != after_snapshot.digest:
            return 0
        keys = {snapshot.key(test) for test in tests}
        keys.discard(None)
        self._load().update(keys)
        if keys:
            self._save()
        return len(keys)
