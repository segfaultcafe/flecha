#!/usr/bin/env python3
"""Run and evaluate Flecha's versioned v0.3 performance workloads."""

from __future__ import annotations

import argparse
import datetime as dt
import html
import json
import os
import platform
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Sequence


ROOT = Path(__file__).resolve().parents[1]
BENCHMARK_ROOT = ROOT / "benchmarks" / "performance"
RESULT_FORMAT = "flecha-performance-v1"
BASELINE_FORMAT = "flecha-performance-baseline-v1"
LANGUAGES = ("flecha", "c", "rust", "go", "python")
COMPETITIVE_WORKLOADS = ("rounds", "collections", "utf8", "fnv")
COMPETITIVE_DIAGNOSTIC_WORKLOADS = ("collections_growth",)
FLECHA_ONLY_WORKLOADS = ("sha256", "buffered")


@dataclass(frozen=True)
class Workload:
    checksum: int
    units: int
    unit: str
    allocation_behavior: str


WORKLOADS = {
    "rounds": Workload(
        4_536_021_502_390_955_919,
        10_000_000,
        "rounds",
        "no workload allocation",
    ),
    "collections": Workload(
        7_999_998_000_000,
        4_000_000,
        "elements",
        "one reserved growable collection containing 4000000 u64 values",
    ),
    "collections_growth": Workload(
        499_999_500_000,
        1_000_000,
        "elements",
        "one growable collection using each language's default growth policy",
    ),
    "utf8": Workload(
        4_194_304,
        4_194_304,
        "code_points",
        "one 81920-byte UTF-8 input retained across 128 traversals",
    ),
    "fnv": Workload(
        6_371_271_965_115_818_789,
        16_777_216,
        "bytes",
        "one 262144-byte input retained across 64 hash traversals",
    ),
    "sha256": Workload(
        14_996,
        524_288,
        "bytes",
        "fixed 4096-byte input and stack-owned SHA-256 state",
    ),
    "buffered": Workload(
        1_048_576,
        1_048_576,
        "bytes",
        "one 257-byte buffered-writer allocation",
    ),
}


@dataclass(frozen=True)
class ProcessResult:
    command: tuple[str, ...]
    returncode: int
    wall_ns: int
    peak_rss_kib: int
    stdout: str
    stderr: str


def _normalize_rss(value: int) -> int:
    if sys.platform == "darwin":
        return value // 1024
    return value


def run_measured(
    command: Sequence[str],
    *,
    cwd: Path,
    timeout_seconds: float,
    env: dict[str, str] | None = None,
) -> ProcessResult:
    """Run one command and collect elapsed time and per-child maximum RSS."""
    resolved_env = os.environ.copy()
    if env:
        resolved_env.update(env)

    if os.name != "posix" or not hasattr(os, "wait4"):
        started = time.perf_counter_ns()
        completed = subprocess.run(
            list(command),
            cwd=cwd,
            env=resolved_env,
            text=True,
            capture_output=True,
            timeout=timeout_seconds,
            check=False,
        )
        return ProcessResult(
            tuple(command),
            completed.returncode,
            time.perf_counter_ns() - started,
            0,
            completed.stdout,
            completed.stderr,
        )

    with tempfile.TemporaryFile() as stdout_file, tempfile.TemporaryFile() as stderr_file:
        started = time.perf_counter_ns()
        pid = os.fork()
        if pid == 0:
            try:
                os.setsid()
                os.chdir(cwd)
                os.dup2(stdout_file.fileno(), 1)
                os.dup2(stderr_file.fileno(), 2)
                os.execvpe(command[0], list(command), resolved_env)
            except BaseException as error:  # pragma: no cover - child-only failure
                os.write(2, f"performance runner exec failed: {error}\n".encode())
                os._exit(127)

        deadline = time.monotonic() + timeout_seconds
        status = 0
        usage = None
        while True:
            waited, status, usage = os.wait4(pid, os.WNOHANG)
            if waited == pid:
                break
            if time.monotonic() >= deadline:
                try:
                    os.killpg(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                _, status, usage = os.wait4(pid, 0)
                raise TimeoutError(
                    f"command exceeded {timeout_seconds:.0f}s: {' '.join(command)}"
                )
            # Keep the timeout poll well below the shortest benchmark kernel.
            # A coarse sleep here becomes part of every measured wall time.
            time.sleep(0.0001)

        elapsed = time.perf_counter_ns() - started
        stdout_file.seek(0)
        stderr_file.seek(0)
        stdout = stdout_file.read().decode("utf-8", errors="replace")
        stderr = stderr_file.read().decode("utf-8", errors="replace")
        return ProcessResult(
            tuple(command),
            os.waitstatus_to_exitcode(status),
            elapsed,
            _normalize_rss(int(usage.ru_maxrss if usage else 0)),
            stdout,
            stderr,
        )


def require_success(result: ProcessResult, purpose: str) -> None:
    if result.returncode == 0:
        return
    raise RuntimeError(
        f"{purpose} failed with exit {result.returncode}\n"
        f"command: {' '.join(result.command)}\n"
        f"stdout:\n{result.stdout}\n"
        f"stderr:\n{result.stderr}"
    )


def integer_median(values: Iterable[int]) -> int:
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) // 2


def percentile(values: Sequence[int], percent: int) -> int:
    """Return a deterministic nearest-rank percentile for a nonempty sample."""
    if not values or percent < 0 or percent > 100:
        raise ValueError("percentile requires samples and a percentage from 0 to 100")
    ordered = sorted(values)
    index = (percent * (len(ordered) - 1) + 50) // 100
    return ordered[index]


def summarize(
    name: str,
    kind: str,
    language: str,
    results: Sequence[ProcessResult],
    *,
    command: Sequence[str],
    units: int = 0,
    unit: str = "invocations",
    allocation_behavior: str = "tool-managed allocations",
) -> dict[str, object]:
    if not results:
        raise ValueError("at least one measurement is required")
    walls = [result.wall_ns for result in results]
    rss = [result.peak_rss_kib for result in results]
    median_wall = integer_median(walls)
    deviations = [abs(value - median_wall) for value in walls]
    return {
        "name": name,
        "kind": kind,
        "language": language,
        "informational": language != "flecha",
        "command": list(command),
        "sample_count": len(results),
        "wall_ns": {
            "minimum": min(walls),
            "median": median_wall,
            "mean": sum(walls) // len(walls),
            "p10": percentile(walls, 10),
            "p90": percentile(walls, 90),
            "median_absolute_deviation": integer_median(deviations),
            "maximum": max(walls),
        },
        "peak_rss_kib": {
            "minimum": min(rss),
            "median": integer_median(rss),
            "maximum": max(rss),
        },
        "work": {
            "units": units,
            "unit": unit,
            "median_units_per_second": (
                units * 1_000_000_000 // median_wall if units else 0
            ),
            "allocation_behavior": allocation_behavior,
        },
        "samples": {"wall_ns": walls, "peak_rss_kib": rss},
    }


def measure_samples(
    command_factory: Callable[[int], tuple[Sequence[str], dict[str, str] | None]],
    *,
    cwd: Path,
    warmups: int,
    samples: int,
    timeout_seconds: float,
    validate: Callable[[ProcessResult], None] | None = None,
) -> tuple[list[ProcessResult], Sequence[str]]:
    last_command: Sequence[str] = ()
    for index in range(warmups):
        command, env = command_factory(-index - 1)
        last_command = command
        result = run_measured(
            command, cwd=cwd, timeout_seconds=timeout_seconds, env=env
        )
        require_success(result, "warmup")
        if validate:
            validate(result)

    results: list[ProcessResult] = []
    for index in range(samples):
        command, env = command_factory(index)
        last_command = command
        result = run_measured(
            command, cwd=cwd, timeout_seconds=timeout_seconds, env=env
        )
        require_success(result, "measurement")
        if validate:
            validate(result)
        results.append(result)
    return results, last_command


def first_line(command: Sequence[str]) -> str:
    try:
        completed = subprocess.run(
            list(command), text=True, capture_output=True, timeout=15, check=False
        )
    except (OSError, subprocess.SubprocessError):
        return "unavailable"
    output = (completed.stdout or completed.stderr).strip().splitlines()
    return output[0] if output else f"exit {completed.returncode}"


def cpu_name() -> str:
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        for line in cpuinfo.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("model name"):
                return line.partition(":")[2].strip()
    return platform.processor() or "unknown"


def tool_path(value: str) -> str:
    found = shutil.which(value)
    if found is None:
        raise FileNotFoundError(f"required tool is unavailable: {value}")
    return str(Path(found).resolve())


def compile_command(
    language: str,
    *,
    output: Path,
    flecha: Path,
    std: Path,
    c_compiler: str,
    rustc: str,
    go: str,
    python: str,
) -> tuple[list[str], dict[str, str] | None]:
    source = BENCHMARK_ROOT / language
    if language == "flecha":
        return (
            [
                str(flecha),
                "build",
                str(source / "main.fl"),
                "--lib",
                f"std={std}",
                "-o",
                str(output),
            ],
            None,
        )
    if language == "c":
        return (
            [c_compiler, "-O2", "-DNDEBUG", "-std=c17", str(source / "main.c"), "-o", str(output)],
            None,
        )
    if language == "rust":
        return (
            [rustc, "-C", "opt-level=2", "-C", "debuginfo=0", str(source / "main.rs"), "-o", str(output)],
            None,
        )
    if language == "go":
        return (
            [go, "build", "-trimpath", "-ldflags=-s -w", "-o", str(output), str(source / "main.go")],
            None,
        )
    if language == "python":
        pycache = output.parent / "pycache"
        return (
            [python, "-m", "py_compile", str(source / "main.py")],
            {"PYTHONPYCACHEPREFIX": str(pycache)},
        )
    raise ValueError(f"unsupported language: {language}")


def runtime_command(language: str, artifact: Path, python: str, workload: str) -> list[str]:
    if language == "python":
        return [python, str(BENCHMARK_ROOT / "python" / "main.py"), workload]
    return [str(artifact), workload]


def benchmark_project(destination: Path, std: Path) -> Path:
    project = destination
    source = project / "src"
    source.mkdir(parents=True, exist_ok=True)
    (project / "longbow.toml").write_text(
        "[package]\n"
        'name = "performance_resolution"\n'
        'version = "0.3.0"\n'
        'kind = "program"\n\n'
        "[dependencies]\n"
        # Longbow dependency paths name the package root; it appends `src`.
        f'std = {{ path = "{std.parent if std.name == "src" else std}" }}\n',
        encoding="utf-8",
    )
    (source / "main.fl").write_text(
        "module main;\n\n"
        "@imports :: lib::std::containers::collections;\n\n"
        "i32 main() {\n"
        "    values := collections::Array<i32>::init();\n"
        "    if values.push(42) != none { return 1; }\n"
        "    return 0;\n"
        "}\n",
        encoding="utf-8",
    )
    return project


def run_suite(args: argparse.Namespace) -> dict[str, object]:
    if args.samples < 1 or args.warmups < 0:
        raise ValueError("samples must be positive and warmups nonnegative")
    build_dir = args.build_dir.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    std = args.std.resolve()
    flecha = args.flecha.resolve()
    longbow = args.longbow.resolve()

    requested = tuple(dict.fromkeys(args.languages.split(",")))
    unknown = sorted(set(requested) - set(LANGUAGES))
    if unknown:
        raise ValueError(f"unsupported languages: {', '.join(unknown)}")
    if "flecha" not in requested:
        raise ValueError("Flecha must be included in every performance run")

    tools = {
        "c": tool_path(args.c_compiler),
        "rust": tool_path(args.rustc),
        "go": tool_path(args.go),
        "python": tool_path(args.python),
    }
    measurements: list[dict[str, object]] = []
    artifacts: dict[str, Path] = {}

    for language in requested:
        artifact = build_dir / "bin" / f"{language}-performance"
        artifact.parent.mkdir(parents=True, exist_ok=True)

        def command_factory(index: int, language: str = language) -> tuple[Sequence[str], dict[str, str] | None]:
            sample = build_dir / "compile" / language / str(index)
            sample.parent.mkdir(parents=True, exist_ok=True)
            command, environment = compile_command(
                language,
                output=sample,
                flecha=flecha,
                std=std,
                c_compiler=tools["c"],
                rustc=tools["rust"],
                go=tools["go"],
                python=tools["python"],
            )
            return command, environment

        compiled, command = measure_samples(
            command_factory,
            cwd=ROOT,
            warmups=args.warmups,
            samples=args.samples,
            timeout_seconds=args.timeout,
        )
        measurements.append(
            summarize(
                f"compile.{language}",
                "compile",
                language,
                compiled,
                command=command,
                units=1,
                unit="translation_units",
                allocation_behavior="compiler-managed peak resident memory",
            )
        )

        if language != "python":
            final_source = build_dir / "compile" / language / str(args.samples - 1)
            shutil.copy2(final_source, artifact)
            artifact.chmod(0o755)
        artifacts[language] = artifact

    for workload in (
        *COMPETITIVE_WORKLOADS,
        *COMPETITIVE_DIAGNOSTIC_WORKLOADS,
        *FLECHA_ONLY_WORKLOADS,
    ):
        specification = WORKLOADS[workload]
        workload_languages = (
            requested
            if workload in (*COMPETITIVE_WORKLOADS, *COMPETITIVE_DIAGNOSTIC_WORKLOADS)
            else ("flecha",)
        )
        commands = {
            language: runtime_command(
                language, artifacts[language], tools["python"], workload
            )
            for language in workload_languages
        }
        runtime_results: dict[str, list[ProcessResult]] = {
            language: [] for language in workload_languages
        }

        def run_runtime(language: str) -> ProcessResult:
            result = run_measured(
                commands[language], cwd=ROOT, timeout_seconds=args.timeout
            )
            require_success(result, f"{language}/{workload}")
            output = result.stdout.strip()
            if output != str(specification.checksum):
                raise RuntimeError(
                    f"{language}/{workload} checksum mismatch: expected "
                    f"{specification.checksum}, got {output!r}"
                )
            return result

        # Rotate a fixed language order for each round. This keeps the campaign
        # reproducible while balancing thermal/frequency drift across languages.
        for round_index in range(args.warmups + args.samples):
            offset = round_index % len(workload_languages)
            order = workload_languages[offset:] + workload_languages[:offset]
            for language in order:
                result = run_runtime(language)
                if round_index >= args.warmups:
                    runtime_results[language].append(result)

        for language in workload_languages:
            measurements.append(
                summarize(
                    f"runtime.{language}.{workload}",
                    "runtime",
                    language,
                    runtime_results[language],
                    command=commands[language],
                    units=specification.units,
                    unit=specification.unit,
                    allocation_behavior=specification.allocation_behavior,
                )
            )

    package_root = build_dir / "package-resolution"

    def package_command(index: int) -> tuple[Sequence[str], dict[str, str] | None]:
        project = benchmark_project(package_root / str(index), std)
        return [str(longbow), "check", str(project)], None

    package_results, package_invocation = measure_samples(
        package_command,
        cwd=ROOT,
        warmups=args.warmups,
        samples=args.samples,
        timeout_seconds=args.timeout,
    )
    measurements.append(
        summarize(
            "package.longbow.check",
            "package_resolution",
            "flecha",
            package_results,
            command=package_invocation,
            units=1,
            unit="resolved_projects",
            allocation_behavior="Longbow resolver and compiler peak resident memory",
        )
    )

    versions = {
        "flecha": first_line([str(flecha), "--version"]),
        "longbow": first_line([str(longbow), "--version"]),
        "c": first_line([tools["c"], "--version"]),
        "rust": first_line([tools["rust"], "--version"]),
        "go": first_line([tools["go"], "version"]),
        "python": first_line([tools["python"], "--version"]),
    }
    report: dict[str, object] = {
        "format": RESULT_FORMAT,
        "suite_version": "0.3.0",
        "generated_at_utc": dt.datetime.now(dt.UTC).isoformat(),
        "policy": {
            "flecha_measurements_release_blocking": True,
            "competitive_runtime_tier_release_blocking": True,
            "competitive_compile_measurements_informational": True,
            "minimum_competitive_release_tier": "C",
            "optimization_target_tier": "B",
            "warmup_count": args.warmups,
            "sample_count": args.samples,
            "timeout_seconds": args.timeout,
            "runtime_measurement_order": "deterministic rotating language order",
        },
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "cpu": cpu_name(),
            "logical_cpus": os.cpu_count() or 0,
            "github_runner_image": os.environ.get("ImageOS", ""),
            "github_runner_version": os.environ.get("ImageVersion", ""),
        },
        "toolchains": versions,
        "measurements": measurements,
    }
    report["competitive_rating"] = competitive_rating(report)
    return report


def result_index(report: dict[str, object]) -> dict[str, dict[str, object]]:
    measurements = report.get("measurements")
    if not isinstance(measurements, list):
        raise ValueError("performance report must contain a measurements array")
    result: dict[str, dict[str, object]] = {}
    for entry in measurements:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            raise ValueError("every measurement requires a string name")
        name = str(entry["name"])
        if name in result:
            raise ValueError(f"duplicate measurement: {name}")
        result[name] = entry
    return result


def competitive_rating(report: dict[str, object]) -> dict[str, object]:
    """Apply the project's runtime-only S/A/B/C/D/F comparison ladder."""
    entries = result_index(report)
    wins = {language: 0 for language in ("c", "rust", "go", "python")}
    ratios: dict[str, dict[str, float]] = {}
    for workload in COMPETITIVE_WORKLOADS:
        flecha = entries.get(f"runtime.flecha.{workload}")
        if flecha is None:
            raise ValueError(f"rating requires runtime.flecha.{workload}")
        flecha_wall = int(flecha["wall_ns"]["median"])
        ratios[workload] = {}
        for language in wins:
            competitor = entries.get(f"runtime.{language}.{workload}")
            if competitor is None:
                raise ValueError(f"rating requires runtime.{language}.{workload}")
            competitor_wall = int(competitor["wall_ns"]["median"])
            ratios[workload][language] = flecha_wall / competitor_wall
            if flecha_wall < competitor_wall:
                wins[language] += 1

    assignment_count = len(COMPETITIVE_WORKLOADS)
    if wins["c"] > assignment_count / 2:
        tier = "S"
        reason = "Flecha beats C on more than half of the runtime assignments."
    elif wins["rust"] > assignment_count / 2:
        tier = "A"
        reason = "Flecha beats Rust on more than half of the runtime assignments."
    elif wins["rust"] >= 1:
        tier = "B"
        reason = "Flecha beats Rust on at least one runtime assignment."
    elif wins["go"] == assignment_count and wins["python"] == assignment_count:
        tier = "C"
        reason = "Flecha beats Go and Python on every runtime assignment."
    elif wins["go"] >= 1:
        tier = "D"
        reason = "Flecha beats Go on at least one runtime assignment."
    else:
        tier = "F"
        reason = "Flecha does not beat Go on any runtime assignment."
    return {
        "tier": tier,
        "reason": reason,
        "assignment_count": assignment_count,
        "wins": wins,
        "flecha_wall_ratio": ratios,
        "rule": {
            "S": "beat C on more than 50% of assignments",
            "A": "beat Rust on more than 50% of assignments",
            "B": "beat Rust on at least one assignment",
            "C": "beat Go and Python on every assignment",
            "D": "beat Go on at least one assignment",
            "F": "beat Go on no assignments",
        },
        "measurement_note": (
            "A win is a strictly lower median optimized runtime. Ratios are retained "
            "so close results can be reviewed instead of overstated."
        ),
    }


def rating_meets_minimum(tier: str, minimum: str) -> bool:
    order = ("S", "A", "B", "C", "D", "F")
    if tier not in order or minimum not in order:
        raise ValueError("competitive tiers must be one of S, A, B, C, D, or F")
    return order.index(tier) <= order.index(minimum)


def validate_baseline(baseline: dict[str, object]) -> None:
    if baseline.get("format") != BASELINE_FORMAT:
        raise ValueError(f"baseline format must be {BASELINE_FORMAT}")
    entries = baseline.get("measurements")
    if not isinstance(entries, dict) or not entries:
        raise ValueError("baseline must contain a nonempty measurements object")
    for name, entry in entries.items():
        if not isinstance(name, str) or not isinstance(entry, dict):
            raise ValueError("invalid baseline measurement")
        for field in (
            "reference_wall_median_ns",
            "maximum_wall_regression_percent",
            "reference_peak_rss_kib",
            "maximum_rss_regression_percent",
        ):
            value = entry.get(field)
            if not isinstance(value, int) or value < 0:
                raise ValueError(f"{name}.{field} must be a nonnegative integer")


def check_baseline(report: dict[str, object], baseline: dict[str, object]) -> list[str]:
    if report.get("format") != RESULT_FORMAT:
        raise ValueError(f"result format must be {RESULT_FORMAT}")
    validate_baseline(baseline)
    results = result_index(report)
    failures: list[str] = []
    entries = baseline["measurements"]
    assert isinstance(entries, dict)
    for name, raw in entries.items():
        assert isinstance(raw, dict)
        measurement = results.get(name)
        if measurement is None:
            failures.append(f"missing release-gate measurement: {name}")
            continue
        wall = measurement.get("wall_ns")
        rss = measurement.get("peak_rss_kib")
        if not isinstance(wall, dict) or not isinstance(rss, dict):
            failures.append(f"invalid measurement summary: {name}")
            continue
        actual_wall = int(wall["median"])
        actual_rss = int(rss["maximum"])
        reference_wall = int(raw["reference_wall_median_ns"])
        reference_rss = int(raw["reference_peak_rss_kib"])
        wall_limit = reference_wall * (
            100 + int(raw["maximum_wall_regression_percent"])
        ) // 100
        rss_limit = reference_rss * (
            100 + int(raw["maximum_rss_regression_percent"])
        ) // 100
        if actual_wall > wall_limit:
            failures.append(
                f"{name} median wall time {actual_wall}ns exceeds {wall_limit}ns "
                f"({raw['maximum_wall_regression_percent']}% over {reference_wall}ns)"
            )
        if actual_rss > rss_limit:
            failures.append(
                f"{name} peak RSS {actual_rss} KiB exceeds {rss_limit} KiB "
                f"({raw['maximum_rss_regression_percent']}% over {reference_rss} KiB)"
            )
    return failures


def make_baseline(
    report: dict[str, object],
    *,
    platform_id: str,
    wall_tolerance: int,
    rss_tolerance: int,
) -> dict[str, object]:
    measurements: dict[str, object] = {}
    for name, entry in result_index(report).items():
        if entry.get("language") != "flecha":
            continue
        wall = entry["wall_ns"]
        rss = entry["peak_rss_kib"]
        assert isinstance(wall, dict) and isinstance(rss, dict)
        measurements[name] = {
            "reference_wall_median_ns": int(wall["median"]),
            "maximum_wall_regression_percent": wall_tolerance,
            "reference_peak_rss_kib": int(rss["maximum"]),
            "maximum_rss_regression_percent": rss_tolerance,
        }
    return {
        "format": BASELINE_FORMAT,
        "release": "0.3.0",
        "platform_id": platform_id,
        "review_policy": (
            "Update only from a reviewed controlled-host qualification run; "
            "competitive-language measurements never set Flecha release thresholds."
        ),
        "source_toolchains": report.get("toolchains", {}),
        "source_host": report.get("host", {}),
        "measurements": measurements,
    }


def milliseconds(value: int) -> str:
    return f"{value / 1_000_000:.3f}"


def write_markdown(report: dict[str, object], path: Path) -> None:
    entries = result_index(report)
    rating = report.get("competitive_rating")
    if not isinstance(rating, dict):
        rating = competitive_rating(report)
    lines = [
        "# Flecha v0.3 performance qualification",
        "",
        f"**Competitive runtime tier: {rating['tier']}** — {rating['reason']}",
        "",
        "The v0.3 release floor is C; B is the optimization target. Versioned Flecha",
        "wall-time and memory baselines remain independent release gates.",
        "",
        "Raw optimized-runtime wins: "
        + ", ".join(
            f"{language} {count}/{rating['assignment_count']}"
            for language, count in rating["wins"].items()
        )
        + ".",
        "",
        "## Toolchains",
        "",
    ]
    for name, value in sorted(dict(report.get("toolchains", {})).items()):
        lines.append(f"- **{name}:** `{value}`")
    lines.extend(
        [
            "",
            "## Compilation",
            "",
            "| Language | Median ms | Peak RSS KiB |",
            "| --- | ---: | ---: |",
        ]
    )
    for language in LANGUAGES:
        entry = entries.get(f"compile.{language}")
        if entry is None:
            continue
        wall = entry["wall_ns"]
        rss = entry["peak_rss_kib"]
        lines.append(
            f"| {language} | {milliseconds(int(wall['median']))} | {int(rss['maximum'])} |"
        )

    lines.extend(
        [
            "",
            "## Competitive runtime kernels",
            "",
            "| Workload | Language | Median ms | P10–P90 ms | Units/s | Peak RSS KiB | vs C |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for workload in COMPETITIVE_WORKLOADS:
        c_entry = entries.get(f"runtime.c.{workload}")
        c_wall = int(c_entry["wall_ns"]["median"]) if c_entry else 0
        for language in LANGUAGES:
            entry = entries.get(f"runtime.{language}.{workload}")
            if entry is None:
                continue
            wall = entry["wall_ns"]
            rss = entry["peak_rss_kib"]
            work = entry["work"]
            ratio = int(wall["median"]) / c_wall if c_wall else 0.0
            lines.append(
                f"| {workload} | {language} | {milliseconds(int(wall['median']))} | "
                f"{milliseconds(int(wall['p10']))}–{milliseconds(int(wall['p90']))} | "
                f"{int(work['median_units_per_second'])} | {int(rss['maximum'])} | "
                f"{ratio:.2f}x |"
            )

    lines.extend(
        [
            "",
            "## Competitive diagnostic workloads (not tier-scored)",
            "",
            "| Workload | Language | Median ms | P10–P90 ms | Peak RSS KiB |",
            "| --- | --- | ---: | ---: | ---: |",
        ]
    )
    for workload in COMPETITIVE_DIAGNOSTIC_WORKLOADS:
        for language in LANGUAGES:
            entry = entries.get(f"runtime.{language}.{workload}")
            if entry is None:
                continue
            wall = entry["wall_ns"]
            rss = entry["peak_rss_kib"]
            lines.append(
                f"| {workload} | {language} | {milliseconds(int(wall['median']))} | "
                f"{milliseconds(int(wall['p10']))}–{milliseconds(int(wall['p90']))} | "
                f"{int(rss['maximum'])} |"
            )

    lines.extend(
        [
            "",
            "## Flecha-only release workloads",
            "",
            "| Measurement | Median ms | Peak RSS KiB |",
            "| --- | ---: | ---: |",
        ]
    )
    for name, entry in entries.items():
        if entry.get("language") != "flecha" or (
            name.startswith("runtime.flecha.")
            and name.rsplit(".", 1)[-1]
            in (*COMPETITIVE_WORKLOADS, *COMPETITIVE_DIAGNOSTIC_WORKLOADS)
        ):
            continue
        wall = entry["wall_ns"]
        rss = entry["peak_rss_kib"]
        lines.append(
            f"| {name} | {milliseconds(int(wall['median']))} | {int(rss['maximum'])} |"
        )
    lines.extend(
        [
            "",
            "## Interpretation",
            "",
            "Do not interpret these numbers as general language rankings. They cover a",
            "small deterministic workload set, include process startup, and use each",
            "language's ordinary optimized toolchain. Python executes through its",
            "interpreter; its compile row is bytecode compilation, not native codegen.",
            "The growth-heavy collection diagnostic is deliberately excluded from tier",
            "scoring but retained to expose allocator and capacity-growth weaknesses.",
            "Shared CI hosts are suitable for broad regression tolerances, not fine-grained",
            "claims. Re-run on a controlled idle host before making optimization decisions.",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def write_svg(report: dict[str, object], path: Path) -> None:
    entries = result_index(report)
    width = 960
    row_height = 24
    rows = len(COMPETITIVE_WORKLOADS) * len(LANGUAGES)
    height = 90 + rows * row_height + len(COMPETITIVE_WORKLOADS) * 28
    output = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        '<style>text{font-family:ui-monospace,monospace;fill:#202124}.title{font-size:20px;font-weight:700}.label{font-size:13px}.note{font-size:12px;fill:#5f6368}</style>',
        '<text x="24" y="32" class="title">Competitive runtime: wall time relative to C</text>',
        '<text x="24" y="54" class="note">Lower is better; informational, includes process startup</text>',
    ]
    colors = {
        "flecha": "#7b2cbf",
        "c": "#4c78a8",
        "rust": "#f58518",
        "go": "#54a24b",
        "python": "#e45756",
    }
    y = 86
    for workload in COMPETITIVE_WORKLOADS:
        c_entry = entries.get(f"runtime.c.{workload}")
        if c_entry is None:
            continue
        c_wall = int(c_entry["wall_ns"]["median"])
        ratios = []
        for language in LANGUAGES:
            entry = entries.get(f"runtime.{language}.{workload}")
            if entry is not None:
                ratios.append((language, int(entry["wall_ns"]["median"]) / c_wall))
        scale = max((ratio for _, ratio in ratios), default=1.0)
        output.append(f'<text x="24" y="{y}" class="label" font-weight="700">{html.escape(workload)}</text>')
        y += 18
        for language, ratio in ratios:
            bar_width = max(2, 620 * ratio / scale)
            output.append(f'<text x="24" y="{y + 13}" class="label">{language}</text>')
            output.append(
                f'<rect x="110" y="{y}" width="{bar_width:.1f}" height="16" rx="2" fill="{colors[language]}"/>'
            )
            output.append(
                f'<text x="{120 + bar_width:.1f}" y="{y + 13}" class="label">{ratio:.2f}x</text>'
            )
            y += row_height
        y += 10
    output.append("</svg>")
    path.write_text("\n".join(output) + "\n", encoding="utf-8")


def load_json(path: Path) -> dict[str, object]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return value


def write_json(value: dict[str, object], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def self_test() -> None:
    synthetic = ProcessResult(("true",), 0, 100, 20, "", "")
    summary = summarize(
        "runtime.flecha.synthetic",
        "runtime",
        "flecha",
        [synthetic, ProcessResult(("true",), 0, 200, 30, "", ""), ProcessResult(("true",), 0, 300, 25, "", "")],
        command=("true",),
        units=100,
    )
    assert summary["wall_ns"]["median"] == 200
    assert summary["peak_rss_kib"]["maximum"] == 30
    report = {"format": RESULT_FORMAT, "measurements": [summary]}
    baseline = make_baseline(
        report, platform_id="self-test", wall_tolerance=10, rss_tolerance=10
    )
    assert check_baseline(report, baseline) == []
    summary["wall_ns"]["median"] = 221
    failures = check_baseline(report, baseline)
    assert len(failures) == 1 and "wall time" in failures[0]

    def rating_report(flecha_times: Sequence[int]) -> dict[str, object]:
        measurements: list[dict[str, object]] = []
        competitor_times = {"c": 80, "rust": 90, "go": 100, "python": 1000}
        for index, workload in enumerate(COMPETITIVE_WORKLOADS):
            for language, wall in (
                ("flecha", flecha_times[index]),
                *competitor_times.items(),
            ):
                measurements.append(
                    {
                        "name": f"runtime.{language}.{workload}",
                        "wall_ns": {"median": wall},
                    }
                )
        return {"format": RESULT_FORMAT, "measurements": measurements}

    assert competitive_rating(rating_report((70, 70, 70, 90)))["tier"] == "S"
    assert competitive_rating(rating_report((85, 85, 85, 95)))["tier"] == "A"
    assert competitive_rating(rating_report((85, 110, 110, 110)))["tier"] == "B"
    assert competitive_rating(rating_report((95, 95, 95, 95)))["tier"] == "C"
    assert competitive_rating(rating_report((95, 105, 105, 105)))["tier"] == "D"
    assert competitive_rating(rating_report((105, 105, 105, 105)))["tier"] == "F"
    print("performance harness self-test passed")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    run = subparsers.add_parser("run", help="compile and measure the workload suite")
    run.add_argument("--flecha", type=Path, required=True)
    run.add_argument("--longbow", type=Path, required=True)
    run.add_argument("--std", type=Path, required=True)
    run.add_argument("--build-dir", type=Path, required=True)
    run.add_argument("--output", type=Path, required=True)
    run.add_argument("--markdown", type=Path)
    run.add_argument("--svg", type=Path)
    run.add_argument("--languages", default=",".join(LANGUAGES))
    run.add_argument("--samples", type=int, default=7)
    run.add_argument("--warmups", type=int, default=1)
    run.add_argument("--timeout", type=float, default=120.0)
    run.add_argument("--c-compiler", default=os.environ.get("CC", "cc"))
    run.add_argument("--rustc", default="rustc")
    run.add_argument("--go", default="go")
    run.add_argument("--python", default=sys.executable)

    check = subparsers.add_parser("check", help="compare results with a baseline")
    check.add_argument("--results", type=Path, required=True)
    check.add_argument("--baseline", type=Path, required=True)

    baseline = subparsers.add_parser("baseline", help="create a reviewed baseline candidate")
    baseline.add_argument("--results", type=Path, required=True)
    baseline.add_argument("--output", type=Path, required=True)
    baseline.add_argument("--platform-id", required=True)
    baseline.add_argument("--wall-tolerance", type=int, default=40)
    baseline.add_argument("--rss-tolerance", type=int, default=25)

    validate = subparsers.add_parser("validate", help="validate a baseline schema")
    validate.add_argument("--baseline", type=Path, required=True)

    competitive = subparsers.add_parser(
        "competitive-check", help="enforce the optimized-runtime release tier"
    )
    competitive.add_argument("--results", type=Path, required=True)
    competitive.add_argument("--minimum", default="C")

    subparsers.add_parser("self-test", help="exercise deterministic evaluation logic")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if args.command == "self-test":
        self_test()
        return 0
    if args.command == "run":
        report = run_suite(args)
        write_json(report, args.output)
        if args.markdown:
            args.markdown.parent.mkdir(parents=True, exist_ok=True)
            write_markdown(report, args.markdown)
        if args.svg:
            args.svg.parent.mkdir(parents=True, exist_ok=True)
            write_svg(report, args.svg)
        print(f"wrote {args.output}")
        return 0
    if args.command == "check":
        failures = check_baseline(load_json(args.results), load_json(args.baseline))
        if failures:
            for failure in failures:
                print(f"performance regression: {failure}", file=sys.stderr)
            return 1
        print("performance baseline check passed")
        return 0
    if args.command == "baseline":
        baseline = make_baseline(
            load_json(args.results),
            platform_id=args.platform_id,
            wall_tolerance=args.wall_tolerance,
            rss_tolerance=args.rss_tolerance,
        )
        write_json(baseline, args.output)
        print(f"wrote baseline candidate {args.output}")
        return 0
    if args.command == "validate":
        validate_baseline(load_json(args.baseline))
        print("performance baseline schema is valid")
        return 0
    if args.command == "competitive-check":
        report = load_json(args.results)
        rating = competitive_rating(report)
        tier = str(rating["tier"])
        if not rating_meets_minimum(tier, args.minimum):
            print(
                f"performance tier {tier} misses required tier {args.minimum}: "
                f"{rating['reason']}",
                file=sys.stderr,
            )
            return 1
        print(f"performance tier {tier} meets required tier {args.minimum}")
        return 0
    raise AssertionError(f"unhandled command: {args.command}")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, TimeoutError, ValueError) as error:
        print(f"performance error: {error}", file=sys.stderr)
        raise SystemExit(2) from error
