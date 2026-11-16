#!/usr/bin/env python3
"""Build and run focused or full CTest validation with conservative local reuse."""

from __future__ import annotations

import argparse
import contextlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

from test_cache import CacheContext
from test_progress import run_logged
from test_selection import SelectionError, changed_files, select_tests

ROOT = Path(__file__).resolve().parents[1]


def default_jobs() -> int:
    try:
        count = len(os.sched_getaffinity(0))
    except AttributeError:
        count = os.cpu_count() or 1
    return min(4, max(1, count))


def positive(value: str) -> int:
    result = int(value)
    if result < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return result


@contextlib.contextmanager
def build_lock(build_dir: Path):
    """One runner owns the build, CTest logs and result cache at a time."""
    path = build_dir / ".flecha-test-runner.lock"
    with path.open("a+b") as handle:
        try:
            if os.name == "nt":
                import msvcrt
                handle.write(b"\0")
                handle.flush()
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise RuntimeError(f"another test runner is using {build_dir}") from error
        try:
            yield
        finally:
            if os.name == "nt":
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle, fcntl.LOCK_UN)


def read_results(path: Path) -> tuple[set[str], set[str], set[str]]:
    passed, failed, skipped = set(), set(), set()
    for case in ET.parse(path).iter("testcase"):
        name = case.get("name")
        if not name or name in passed | failed | skipped:
            raise ValueError("JUnit contains a missing or duplicate test name")
        if case.get("status", "run") not in {"run", "notrun", "disabled"}:
            raise ValueError(f"JUnit contains an unknown status for {name}")
        if case.find("skipped") is not None or case.get("status") in {"notrun", "disabled"}:
            skipped.add(name)
        elif case.find("failure") is not None or case.find("error") is not None:
            failed.add(name)
        else:
            passed.add(name)
    return passed, failed, skipped


def choose_tests(repo: Path, tests: list[dict], args) -> tuple[list[dict], list[str], list[str]]:
    paths = []
    if args.full:
        selected, reasons = list(tests), ["full integration suite requested"]
    else:
        try:
            paths = args.changed if args.changed is not None else changed_files(repo, args.base)
            selected, reasons = select_tests(repo, tests, paths)
        except SelectionError as error:
            selected, reasons = list(tests), [f"full fallback: {error}"]
    names = {test["name"] for test in selected}
    for expression in args.regression:
        try:
            pattern = re.compile(expression)
        except re.error as error:
            raise ValueError(f"invalid regression filter: {expression}: {error}") from error
        matches = [test for test in tests if pattern.search(test["name"])]
        if not matches:
            raise ValueError(f"regression filter matched no tests: {expression}")
        names.update(test["name"] for test in matches)
        reasons.append(f"explicit regression filter: {expression}")
    return [test for test in tests if test["name"] in names], reasons, paths


def discover(ctest: str, build: Path) -> list[dict]:
    result = subprocess.run([ctest, "--test-dir", str(build), "--show-only=json-v1"],
                            capture_output=True, text=True, check=True)
    inventory = json.loads(result.stdout)
    tests = inventory.get("tests") if isinstance(inventory, dict) else None
    if not isinstance(tests, list) or any(
            not isinstance(test, dict) or not isinstance(test.get("name"), str)
            or not test["name"] or not isinstance(test.get("command"), list)
            or not test["command"] or not all(isinstance(arg, str) for arg in test["command"])
            for test in tests):
        raise ValueError("CTest discovery has missing test names or commands")
    if not tests:
        raise RuntimeError("CTest discovered no tests; configure with BUILD_TESTING=ON")
    if len({test["name"] for test in tests}) != len(tests):
        raise ValueError("CTest discovery contains duplicate test names")
    return tests


def run(args) -> int:
    started = time.monotonic()
    build = args.build_dir.resolve()
    if args.configure:
        build.mkdir(parents=True, exist_ok=True)
    elif not (build / "CMakeCache.txt").is_file():
        raise RuntimeError(f"configure the test build first: make configure-tests TEST_BUILD_DIR={build}")
    with build_lock(build):
        return run_locked(args, build, started)


def run_locked(args, build: Path, started: float) -> int:
    report_path = args.report.resolve() if args.report else build / "test-results" / "latest.json"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    log_dir = build / "test-results"
    log_dir.mkdir(exist_ok=True)
    stamp = f"{time.time_ns()}-{os.getpid()}"
    log_path = log_dir / f"{stamp}.log"
    junit = log_dir / f"{stamp}.xml"
    report = {"format": "flecha-test-run-v1", "mode": "full" if args.full else "changed",
              "jobs": args.jobs, "build_dir": str(build), "log": str(log_path),
              "cache_enabled": not args.no_cache, "timings": {}}
    print(f"Log: {log_path}", flush=True)
    try:
        with log_path.open("w") as log:
            if args.configure:
                before = time.monotonic()
                command = [args.cmake, "-S", str(ROOT), "-B", str(build),
                           "-DBUILD_TESTING=ON", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
                if args.generator:
                    command += ["-G", args.generator]
                result_code = run_logged(command, log=log, label="Configuring", quiet=args.quiet_progress)
                report["timings"]["configure_seconds"] = time.monotonic() - before
                if result_code:
                    raise RuntimeError(f"configure failed ({result_code}); see {log_path}")
            if not args.list:
                before = time.monotonic()
                command = [args.cmake, "--build", str(build), "--parallel", str(args.build_jobs or args.jobs)]
                result_code = run_logged(command, log=log, label="Building", quiet=args.quiet_progress)
                report["timings"]["build_seconds"] = time.monotonic() - before
                if result_code:
                    raise RuntimeError(f"build failed ({result_code}); see {log_path}")
            before = time.monotonic()
            tests = discover(args.ctest, build)
            selected, reasons, paths = choose_tests(ROOT, tests, args)
            report.update(discovered=len(tests), selected=len(selected), reasons=reasons,
                          changed_files=paths, selected_tests=[test["name"] for test in selected])
            report["timings"]["selection_seconds"] = time.monotonic() - before
            if args.list:
                report.update(status="planned", executed=0, cached=0)
                print("\n".join(reasons))
                print("\n".join(test["name"] for test in selected))
                return 0
            help_text = subprocess.check_output([args.ctest, "--help"], text=True)
            has_junit = "--output-junit" in help_text
            cache = CacheContext(ROOT, build, ctest=args.ctest)
            before = time.monotonic()
            snapshot = cache.snapshot(tests) if not args.no_cache and selected and has_junit else None
            report["timings"]["cache_lookup_seconds"] = time.monotonic() - before
            if not snapshot and not args.no_cache:
                report["cache_disabled_reason"] = (cache.disabled_reason if has_junit else
                                                   "CTest before 3.21 has no JUnit results")
            cached = [test for test in selected if snapshot and not args.refresh and cache.lookup(test, snapshot)]
            cached_names = {test["name"] for test in cached}
            pending = [test for test in selected if test["name"] not in cached_names]
            report.update(cached=len(cached), cached_tests=sorted(cached_names), executed=len(pending))
            print(f"Selected {len(selected)}/{len(tests)} tests; {len(cached)} cached; "
                  f"running {len(pending)} with {args.jobs} workers.", flush=True)
            passed, failed, skipped = set(), set(), set()
            result_code = 0
            before = time.monotonic()
            if pending:
                if snapshot:
                    # A forced run revokes earlier evidence even if interrupted
                    # or CTest fails before it can produce a results file.
                    cache.invalidate(pending, snapshot)
                command = [args.ctest, "--test-dir", str(build), "--parallel", str(args.jobs),
                           "--output-on-failure", "--no-tests=error"]
                if has_junit:
                    command += ["--output-junit", str(junit)]
                if len(pending) != len(tests):
                    if "--tests-from-file" in help_text:
                        names_file = log_dir / f"{stamp}.tests"
                        names_file.write_text("\n".join(test["name"] for test in pending) + "\n")
                        command += ["--tests-from-file", str(names_file)]
                    else:
                        expression = "^(" + "|".join(re.escape(test["name"]) for test in pending) + ")$"
                        if len(expression) > 24000:
                            raise RuntimeError("selected test names exceed portable argument limit; use CTest 3.29+")
                        command += ["-R", expression]
                report["command"] = command
                result_code = run_logged(command, log=log, label="Testing", total=len(pending),
                                         quiet=args.quiet_progress)
                if has_junit:
                    if not junit.exists():
                        raise RuntimeError(f"CTest did not write results; see {log_path}")
                    passed, failed, skipped = read_results(junit)
                    expected = {test["name"] for test in pending}
                    if passed | failed | skipped != expected:
                        raise RuntimeError("CTest results do not match selected tests; no results cached")
                elif result_code == 0:
                    report["result_detail"] = "CTest passed; individual results require CTest 3.21+"
            report["timings"]["test_seconds"] = time.monotonic() - before
            before = time.monotonic()
            if snapshot:
                after_tests = discover(args.ctest, build)
                if after_tests != tests:
                    raise RuntimeError("CTest definitions changed during validation; rerun required")
                after = cache.snapshot(after_tests)
                if after is None or after.digest != snapshot.digest:
                    raise RuntimeError("validation inputs changed or became unreadable during the run; rerun required")
                report["cache_written"] = cache.record_successes(
                    [test for test in pending if test["name"] in passed], snapshot, after)
            report["timings"]["cache_write_seconds"] = time.monotonic() - before
            report.update(status="passed" if result_code == 0 and not failed else "failed",
                          passed=len(passed), failed_tests=sorted(failed), skipped_tests=sorted(skipped))
            if failed:
                print("Failed: " + ", ".join(sorted(failed)))
            return result_code or (1 if failed else 0)
    except KeyboardInterrupt:
        report.update(status="interrupted")
        raise
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, ET.ParseError) as error:
        report.update(status="error", error=str(error))
        raise
    finally:
        report["timings"]["total_seconds"] = time.monotonic() - started
        report_path.write_text(json.dumps(report, indent=2) + "\n")
        print(f"{report.get('status', 'error').upper()}: {report['timings']['total_seconds']:.2f}s; "
              f"report {report_path}; log {log_path}", flush=True)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-tests")
    parser.add_argument("--jobs", type=positive, default=default_jobs())
    parser.add_argument("--build-jobs", type=positive)
    parser.add_argument("--configure", action="store_true", help="configure under the build lock before building")
    parser.add_argument("--generator", help="CMake generator when configuring")
    parser.add_argument("--full", action="store_true", help="select every test (CI integration gate)")
    parser.add_argument("--base", default="master", help="base ref for committed branch changes")
    parser.add_argument("--changed", action="append", help="explicit changed path; repeatable, replaces Git diff")
    parser.add_argument("--regression", action="append", default=[], help="additional test-name regex; repeatable")
    cache = parser.add_mutually_exclusive_group()
    cache.add_argument("--no-cache", action="store_true", help="neither read nor write successful result cache")
    cache.add_argument("--refresh", action="store_true", help="execute selected tests even on cache hits")
    parser.add_argument("--list", action="store_true", help="explain selection without building or running tests")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--quiet-progress", action="store_true", help="hide live progress; retain summary and logs")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ctest", default="ctest")
    args = parser.parse_args(argv)
    if args.full and args.changed is not None:
        parser.error("--full and --changed cannot be combined")
    if args.list and args.configure:
        parser.error("--list uses an existing build and cannot be combined with --configure")
    return args


if __name__ == "__main__":
    try:
        sys.exit(run(parse_args()))
    except KeyboardInterrupt:
        print("test runner: interrupted", file=sys.stderr)
        sys.exit(130)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, ET.ParseError) as error:
        print(f"test runner: {error}", file=sys.stderr)
        sys.exit(2)
