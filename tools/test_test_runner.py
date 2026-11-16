"""Runner contract tests with fake build/CTest processes and a controlled cache.

These exercise orchestration and report files without compiling Flecha or
running its tests. Cache dependency hashing has its own separate tests.
"""

import contextlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import test_runner as runner


class RunnerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="runner test ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / "build"
        self.build.mkdir()
        (self.build / "CMakeCache.txt").write_text("BUILD_TESTING:BOOL=ON\n")
        self.inventory = [
            {"name": "ParserTests.Basic", "command": ["test_flecha"]},
            {"name": "DocumentationExampleTests.CheckBuildRun", "command": ["cmake"]},
        ]
        self.discovery = {"tests": self.inventory}
        self.help_text = "--output-junit --tests-from-file"
        self.xml = None  # None generates successful cases for requested names.
        self.write_junit = True
        self.exit_code = 0
        self.build_exit_code = 0
        self.configure_exit_code = 0
        self.commands = []
        self.cached = set()
        self.invalidated = []
        self.recorded = []
        self.before_test = None
        self.before_configure = None
        self.snapshot = SimpleNamespace(digest="unchanged")
        self.cache = mock.Mock()
        self.cache.disabled_reason = "unsupported host"
        self.cache.snapshot.return_value = self.snapshot
        self.cache.lookup.side_effect = lambda test, snapshot: test["name"] in self.cached
        self.cache.invalidate.side_effect = self.invalidate
        self.cache.record_successes.side_effect = self.record
        self.cache_factory = self.enter(mock.patch.object(runner, "CacheContext", return_value=self.cache))
        self.enter(mock.patch.object(runner, "ROOT", self.root))
        self.enter(mock.patch.object(runner.subprocess, "run", side_effect=self.process))
        self.enter(mock.patch.object(runner, "run_logged",
                                     side_effect=lambda command, **kwargs: self.process(command).returncode))
        self.enter(mock.patch.object(runner.subprocess, "check_output", side_effect=lambda *a, **kw: self.help_text))
        self.enter(contextlib.redirect_stdout(io.StringIO()))

    def enter(self, manager):
        value = manager.__enter__()
        self.addCleanup(manager.__exit__, None, None, None)
        return value

    def invalidate(self, tests, snapshot):
        self.invalidated.extend(test["name"] for test in tests)
        self.cached.difference_update(test["name"] for test in tests)

    def record(self, tests, snapshot, after):
        if not after or after.digest != snapshot.digest:
            return 0
        self.recorded.extend(test["name"] for test in tests)
        return len(tests)

    def process(self, command, **kwargs):
        self.commands.append(command)
        if command[0] == "fake-cmake":
            if "-S" in command:
                if self.before_configure:
                    self.before_configure()
                return subprocess.CompletedProcess(command, self.configure_exit_code)
            return subprocess.CompletedProcess(command, self.build_exit_code)
        if "--show-only=json-v1" in command:
            return subprocess.CompletedProcess(command, 0, json.dumps(self.discovery), "")
        if self.before_test:
            self.before_test()
        if self.write_junit and "--output-junit" in command:
            output = Path(command[command.index("--output-junit") + 1])
            if "--tests-from-file" in command:
                names = Path(command[command.index("--tests-from-file") + 1]).read_text().splitlines()
            elif "-R" in command:
                import re
                expression = command[command.index("-R") + 1]
                names = [test["name"] for test in self.inventory if re.search(expression, test["name"])]
            else:
                names = [test["name"] for test in self.inventory]
            xml = self.xml if self.xml is not None else "<testsuite>" + "".join(
                f'<testcase name="{name}" status="run"/>' for name in names) + "</testsuite>"
            output.write_text(xml)
        return subprocess.CompletedProcess(command, self.exit_code)

    def args(self, *extra):
        return runner.parse_args(["--build-dir", str(self.build), "--cmake", "fake-cmake",
                                  "--ctest", "fake-ctest", "--full", *extra])

    def report(self):
        return json.loads((self.build / "test-results/latest.json").read_text())

    def test_success_records_passes_and_build_and_test_worker_limits(self):
        self.assertEqual(runner.run(self.args("--jobs", "2", "--build-jobs", "3")), 0)
        self.assertEqual(self.recorded, [test["name"] for test in self.inventory])
        self.assertEqual(self.commands[0][-2:], ["--parallel", "3"])
        command = self.report()["command"]
        self.assertEqual(command[command.index("--parallel") + 1], "2")
        self.assertEqual(self.report()["status"], "passed")

    def test_cache_hit_is_excluded_from_ctest_and_all_hits_skip_execution(self):
        self.cached.add("ParserTests.Basic")
        self.assertEqual(runner.run(self.args()), 0)
        self.assertEqual(self.report()["cached"], 1)
        command = self.report()["command"]
        names = Path(command[command.index("--tests-from-file") + 1]).read_text().splitlines()
        self.assertEqual(names, ["DocumentationExampleTests.CheckBuildRun"])
        self.cached.update(test["name"] for test in self.inventory)
        self.commands.clear()
        self.assertEqual(runner.run(self.args()), 0)
        self.assertEqual(self.report()["executed"], 0)
        self.assertTrue(all("--output-on-failure" not in command for command in self.commands))

    def test_failure_caches_only_successful_sibling(self):
        self.exit_code = 8
        self.xml = ('<testsuite><testcase name="ParserTests.Basic"><failure>bad</failure></testcase>'
                    '<testcase name="DocumentationExampleTests.CheckBuildRun"/></testsuite>')
        self.assertEqual(runner.run(self.args()), 8)
        self.assertEqual(self.recorded, ["DocumentationExampleTests.CheckBuildRun"])
        self.assertEqual(self.report()["status"], "failed")

    def test_skipped_test_is_reported_but_never_cached(self):
        self.xml = ('<testsuite><testcase name="ParserTests.Basic"><skipped/></testcase>'
                    '<testcase name="DocumentationExampleTests.CheckBuildRun"/></testsuite>')
        self.assertEqual(runner.run(self.args()), 0)
        self.assertEqual(self.recorded, ["DocumentationExampleTests.CheckBuildRun"])
        self.assertEqual(self.report()["skipped_tests"], ["ParserTests.Basic"])

    def test_failed_refresh_invalidates_old_success_before_starting_ctest(self):
        self.cached.update(test["name"] for test in self.inventory)
        self.write_junit = False
        self.exit_code = 8
        self.before_test = lambda: self.assertEqual(self.cached, set())
        with self.assertRaises(RuntimeError):
            runner.run(self.args("--refresh"))
        self.assertEqual(self.recorded, [])
        self.assertEqual(self.report()["status"], "error")

    def test_missing_malformed_mismatched_and_duplicate_results_are_not_cached(self):
        bad_results = [
            "<broken", "<testsuite/>",
            '<testsuite><testcase name="Unexpected"/></testsuite>',
            '<testsuite><testcase/></testsuite>',
            '<testsuite><testcase name="ParserTests.Basic"/><testcase name="ParserTests.Basic"><failure/></testcase>'
            '<testcase name="DocumentationExampleTests.CheckBuildRun"/></testsuite>',
        ]
        for xml in bad_results:
            with self.subTest(xml=xml):
                self.xml = xml
                self.recorded.clear()
                with self.assertRaises((ValueError, RuntimeError, runner.ET.ParseError)):
                    runner.run(self.args())
                self.assertEqual(self.recorded, [])
                self.assertEqual(self.report()["status"], "error")

    def test_no_discovered_tests_and_malformed_discovery_fail_closed(self):
        for discovery in [{"tests": []}, {}, {"tests": [{"command": []}]}]:
            with self.subTest(discovery=discovery):
                self.discovery = discovery
                with self.assertRaises((RuntimeError, ValueError)):
                    runner.run(self.args())
                self.assertEqual(self.report()["status"], "error")
        self.cache.record_successes.assert_not_called()

    def test_no_cache_does_not_lookup_or_write_cache(self):
        self.assertEqual(runner.run(self.args("--no-cache")), 0)
        self.cache.snapshot.assert_not_called()
        self.cache.lookup.assert_not_called()
        self.cache.record_successes.assert_not_called()

    def test_changed_inputs_prevent_recording(self):
        self.snapshot.inputs = {"docs/report.md": "old"}
        self.cache.snapshot.side_effect = [self.snapshot, SimpleNamespace(
            digest="changed", inputs={"docs/report.md": "new"})]
        with self.assertRaisesRegex(RuntimeError, "docs/report.md.*2 tests passed, 0 failed"):
            runner.run(self.args())
        self.assertEqual(self.recorded, [])
        self.assertEqual(self.report()["status"], "error")
        self.assertEqual(self.report()["passed"], 2)
        self.assertEqual(self.report()["changed_inputs"], ["docs/report.md"])

    def test_old_ctest_runs_without_cache_and_exact_regex_subset(self):
        self.help_text = ""
        self.assertEqual(runner.run(self.args()), 0)
        self.cache.snapshot.assert_not_called()
        self.assertEqual(self.report()["status"], "passed")
        self.help_text = "--output-junit"
        self.cached.add("ParserTests.Basic")
        self.assertEqual(runner.run(self.args()), 0)
        command = self.report()["command"]
        self.assertEqual(command[command.index("-R") + 1],
                         r"^(DocumentationExampleTests\.CheckBuildRun)$")

    def test_build_failure_never_discovers_or_runs_tests(self):
        self.build_exit_code = 1
        with self.assertRaises(RuntimeError):
            runner.run(self.args())
        self.assertEqual(len(self.commands), 1)
        self.cache.snapshot.assert_not_called()
        self.assertEqual(self.report()["status"], "error")

    def test_configure_precedes_build_and_holds_build_lock(self):
        # A fresh test build is permitted only with --configure.
        (self.build / "CMakeCache.txt").unlink()

        def assert_configure_is_locked():
            with self.assertRaises(RuntimeError):
                with runner.build_lock(self.build):
                    self.fail("configure ran outside the build lock")

        self.before_configure = assert_configure_is_locked
        self.assertEqual(runner.run(self.args("--configure", "--generator", "Ninja")), 0)
        configure = self.commands[0]
        self.assertIn("-S", configure)
        self.assertIn("-DBUILD_TESTING=ON", configure)
        self.assertIn("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", configure)
        self.assertEqual(configure[configure.index("-G") + 1], "Ninja")
        self.assertIn("--build", self.commands[1])
        self.assertIn("configure_seconds", self.report()["timings"])

    def test_configure_failure_stops_before_build_discovery_and_execution(self):
        self.configure_exit_code = 1
        with self.assertRaisesRegex(RuntimeError, "configure failed"):
            runner.run(self.args("--configure"))
        self.assertEqual(len(self.commands), 1)
        self.assertIn("-S", self.commands[0])
        self.cache.snapshot.assert_not_called()
        self.assertEqual(self.report()["status"], "error")

    def test_list_cannot_configure_or_mutate_a_build(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as caught:
                self.args("--list", "--configure")
        self.assertEqual(caught.exception.code, 2)
        self.assertEqual(self.commands, [])

    def test_changed_discovery_aborts_before_any_cache_write(self):
        def change_definition():
            self.discovery = {"tests": [dict(test) for test in self.inventory]}
            self.discovery["tests"][0]["command"] = ["different-test-executable"]

        self.before_test = change_definition
        with self.assertRaisesRegex(RuntimeError, "CTest definitions changed"):
            runner.run(self.args())
        self.assertEqual(self.recorded, [])
        self.cache.record_successes.assert_not_called()
        self.assertEqual(self.cache.snapshot.call_count, 1)
        self.assertEqual(self.report()["status"], "error")

    def test_list_only_does_not_build_run_or_inspect_cache(self):
        self.assertEqual(runner.run(self.args("--list")), 0)
        self.assertEqual(len(self.commands), 1)
        self.assertIn("--show-only=json-v1", self.commands[0])
        self.cache_factory.assert_not_called()
        self.assertEqual(self.report()["status"], "planned")

    def test_regression_filters_are_additive_and_unmatched_filters_fail(self):
        args = self.args()
        args.full = False
        args.changed = ["docs/testing.md"]
        args.regression = [r"^ParserTests\."]
        selected, reasons, paths = runner.choose_tests(self.root, self.inventory, args)
        self.assertEqual(selected, self.inventory)
        args.regression = ["DoesNotExist"]
        with self.assertRaises(ValueError):
            runner.choose_tests(self.root, self.inventory, args)
        args.regression = ["["]
        with self.assertRaises(ValueError):
            runner.choose_tests(self.root, self.inventory, args)

    def test_empty_change_selection_does_not_invoke_ctest_execution(self):
        args = self.args()
        args.full = False
        args.changed = []
        self.assertEqual(runner.run(args), 0)
        self.assertEqual(self.report()["selected"], 0)
        self.assertEqual(self.report()["executed"], 0)
        self.cache.lookup.assert_not_called()
        self.assertTrue(all("--output-on-failure" not in command for command in self.commands))

    def test_git_failure_falls_back_to_full_suite(self):
        args = self.args()
        args.full = False
        with mock.patch.object(runner, "changed_files", side_effect=runner.SelectionError("missing base")):
            selected, reasons, paths = runner.choose_tests(self.root, self.inventory, args)
        self.assertEqual(selected, self.inventory)
        self.assertIn("full fallback", reasons[0])

    def test_build_lock_rejects_overlapping_runner_and_releases_after_error(self):
        with runner.build_lock(self.build):
            with self.assertRaises(RuntimeError):
                with runner.build_lock(self.build):
                    self.fail("concurrent lock unexpectedly succeeded")
        with self.assertRaisesRegex(RuntimeError, "inside lock"):
            with runner.build_lock(self.build):
                raise RuntimeError("inside lock")
        with runner.build_lock(self.build):
            pass


if __name__ == "__main__":
    unittest.main()
