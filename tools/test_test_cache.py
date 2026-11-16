"""Regression tests for result reuse; no compiler build or CTest suite required."""

import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from test_cache import CacheContext, changed_inputs, eligible_test


def sample_test(name="ParserTests.ParsesEmptyCompilationUnit"):
    return {"name": name,
            "command": ["/build/test_flecha", "--gtest_filter=" + name,
                        "--gtest_also_run_disabled_tests"],
            "properties": [{"name": "WORKING_DIRECTORY", "value": "/build"}]}


def wrapped_test():
    test = sample_test()
    test["command"] = ["/usr/bin/cmake", "-D", "TEST_EXECUTABLE=/build/test_flecha",
                       "-D", "TEST_EXECUTOR=", "-D", "TEST_FILTER=" + test["name"],
                       "-D", "TEST_XML_OUTPUT=", "-D", "TEST_EXTRA_ARGS=", "-P",
                       "/usr/share/cmake/Modules/GoogleTest/LaunchTest.cmake"]
    test["properties"].append({"name": "DEF_SOURCE_LINE", "value": "/source/parser.cpp:1"})
    return test


class EligibilityTests(unittest.TestCase):
    def test_only_exact_reviewed_single_tests_are_eligible(self):
        self.assertTrue(eligible_test(sample_test()))
        self.assertFalse(eligible_test(sample_test("RuntimeProcessTests.Spawn")))
        self.assertFalse(eligible_test(sample_test("ParserTests.*")))
        test = sample_test()
        test["command"] = ["/usr/bin/cmake", "--gtest_filter=" + test["name"]]
        self.assertFalse(eligible_test(test))
        test = sample_test()
        test["command"].append("--gtest_repeat=0")
        self.assertFalse(eligible_test(test))

    def test_fixtures_environment_and_changed_execution_semantics_disable_cache(self):
        for property_name in ("FIXTURES_REQUIRED", "FIXTURES_SETUP", "FIXTURES_CLEANUP",
                              "DEPENDS", "ENVIRONMENT", "WILL_FAIL", "DISABLED"):
            with self.subTest(property=property_name):
                test = sample_test()
                test["properties"].append({"name": property_name, "value": "something"})
                self.assertFalse(eligible_test(test))

    def test_cmake_standard_launcher_requires_exact_filter_and_no_extra_executor(self):
        self.assertTrue(eligible_test(wrapped_test()))
        for index, value in [(4, "TEST_EXECUTOR=valgrind"), (6, "TEST_FILTER=ParserTests.*"),
                             (8, "TEST_XML_OUTPUT=/tmp/results.xml"),
                             (10, "TEST_EXTRA_ARGS=--gtest_repeat=0"),
                             (12, "/other/custom.cmake")]:
            with self.subTest(argument=value):
                test = wrapped_test()
                test["command"][index] = value
                self.assertFalse(eligible_test(test))


class SnapshotTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "source"
        self.build = self.source / "custom-build"
        self.build.mkdir(parents=True)
        self.source_file = self.source / "parser.cpp"
        self.source_file.write_text("first source")
        self.binary = self.build / "test_flecha"
        self.binary.write_text("binary")
        self.library = self.root / "runtime.so"
        self.library.write_text("library")
        self.compiler = self.root / "compiler"
        self.compiler.write_text("compiler")
        self.compiler.chmod(0o755)
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_C_COMPILER:FILEPATH={self.compiler}\n"
            f"CMAKE_CXX_COMPILER:FILEPATH={self.compiler}\n")
        (self.build / "compile_commands.json").write_text("[]")
        self.test = sample_test()
        self.test["command"][0] = str(self.binary)
        self.context = CacheContext(self.source, self.build,
                                    environment={"PATH": os.defpath, "TOKEN": "secret"},
                                    ctest=str(self.compiler))
        self.process_context = {"limits": {"RLIMIT_STACK": [8388608, -1]}, "uid": 1000}
        self.patches = [patch("test_cache.platform.system", return_value="Linux"),
                        patch.object(self.context, "_source_files", return_value=[self.source_file]),
                        patch.object(self.context, "_shared_libraries", return_value=[self.library]),
                        patch.object(self.context, "_process_context", return_value=self.process_context)]
        for mock in self.patches:
            mock.start()
            self.addCleanup(mock.stop)

    def snapshot(self):
        result = self.context.snapshot([self.test])
        self.assertIsNotNone(result, self.context.disabled_reason)
        return result

    def test_every_execution_dependency_and_environment_change_invalidates_success(self):
        before = self.snapshot()
        self.context.record_successes([self.test], before, before)
        self.assertTrue(self.context.lookup(self.test, before))
        for path in (self.binary, self.library, self.compiler,
                     self.build / "CMakeCache.txt", self.build / "compile_commands.json"):
            with self.subTest(dependency=path.name):
                original = path.read_text()
                path.write_text(original + "\nchanged")
                changed = self.snapshot()
                self.assertFalse(self.context.lookup(self.test, changed))
                path.write_text(original)
        self.context.environment["TOKEN"] = "different secret"
        self.assertFalse(self.context.lookup(self.test, self.snapshot()))

    def test_source_edit_reuses_unchanged_built_program_but_changes_mutation_guard(self):
        before = self.snapshot()
        self.context.record_successes([self.test], before, before)
        self.source_file.write_text("unrelated documentation/source change")
        after = self.snapshot()
        self.assertNotEqual(before.digest, after.digest)
        self.assertTrue(self.context.lookup(self.test, after))
        # A real implementation change rebuilt into the binary revokes reuse.
        self.binary.write_text("rebuilt implementation")
        self.assertFalse(self.context.lookup(self.test, self.snapshot()))

    def test_test_properties_and_commands_are_part_of_the_key(self):
        before = self.snapshot()
        self.context.record_successes([self.test], before, before)
        self.test["properties"][0]["value"] = "/different-directory"
        self.assertFalse(self.context.lookup(self.test, self.snapshot()))
        self.test["command"][-1] = "--unknown-option"
        self.assertIsNone(self.snapshot().key(self.test))

    def test_changed_inputs_during_run_never_record_a_success(self):
        before = self.snapshot()
        self.source_file.write_text("edit during tests")
        after = self.snapshot()
        self.assertEqual(self.context.record_successes([self.test], before, after), 0)
        self.assertFalse(self.context.lookup(self.test, before))

    def test_changed_input_diagnostics_name_files_without_exposing_secrets(self):
        before = self.snapshot()
        self.source_file.write_text("edit during validation")
        self.context.environment["TOKEN"] = "new secret value"
        changes = changed_inputs(before, self.snapshot())
        self.assertIn(str(self.source_file), changes)
        self.assertIn("<environment>", changes)
        self.assertNotIn("secret", str(changes))
        self.assertEqual(changed_inputs(before, None), ["dependency inspection became unavailable"])

    def test_missing_binary_or_unresolved_libraries_fail_closed(self):
        self.binary.unlink()
        self.assertIsNone(self.context.snapshot([self.test]))
        self.assertIsNotNone(self.context.disabled_reason)

    def test_gtest_sharding_and_loader_injection_disable_reuse(self):
        for variable in ("GTEST_TOTAL_SHARDS", "GTEST_REPEAT", "LD_PRELOAD", "LD_AUDIT",
                         "ASAN_OPTIONS", "LSAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS",
                         "MSAN_OPTIONS", "HWASAN_OPTIONS"):
            with self.subTest(variable=variable):
                self.context.environment[variable] = "1"
                self.assertIsNone(self.context.snapshot([self.test]))
                del self.context.environment[variable]

    def test_generated_test_definitions_and_resource_limits_invalidate_success(self):
        definition = self.build / "CTestTestfile.cmake"
        definition.write_text("original CTest command")
        before = self.snapshot()
        self.context.record_successes([self.test], before, before)
        definition.write_text("changed CTest command")
        self.assertFalse(self.context.lookup(self.test, self.snapshot()))
        definition.write_text("original CTest command")
        self.process_context["limits"]["RLIMIT_STACK"] = [1024, -1]
        self.assertFalse(self.context.lookup(self.test, self.snapshot()))

    def test_changed_discovery_inventory_changes_global_snapshot_digest(self):
        before = self.snapshot()
        self.test["properties"][0]["value"] = "/changed-working-directory"
        self.assertNotEqual(before.digest, self.snapshot().digest)

    def test_cmake_launcher_binary_and_script_are_dependencies(self):
        self.test = wrapped_test()
        launcher = self.root / "GoogleTest" / "LaunchTest.cmake"
        launcher.parent.mkdir()
        launcher.write_text("standard launcher script")
        cmake = self.root / "cmake"
        cmake.write_text("cmake executable")
        self.test["command"][0] = str(cmake)
        self.test["command"][2] = "TEST_EXECUTABLE=" + str(self.binary)
        self.test["command"][12] = str(launcher)
        before = self.snapshot()
        self.context.record_successes([self.test], before, before)
        launcher.write_text("modified launcher script")
        self.assertFalse(self.context.lookup(self.test, self.snapshot()))
        launcher.write_text("standard launcher script")
        cmake.write_text("modified cmake executable")
        self.assertFalse(self.context.lookup(self.test, self.snapshot()))

    def test_only_success_hashes_are_persisted_and_failed_refresh_forgets_success(self):
        before = self.snapshot()
        self.assertEqual(self.context.record_successes([self.test], before, before), 1)
        contents = self.context.path.read_text()
        self.assertNotIn("secret", contents)
        self.assertNotIn("TOKEN", contents)
        self.assertNotIn(self.test["name"], contents)
        other = CacheContext(self.source, self.build)
        self.assertTrue(other.lookup(self.test, before))
        self.context.invalidate([self.test], before)
        self.assertFalse(self.context.lookup(self.test, before))
        self.assertFalse(CacheContext(self.source, self.build).lookup(self.test, before))

    def test_corrupt_cache_is_a_miss(self):
        self.context.path.write_text("broken {")
        self.assertFalse(self.context.lookup(self.test, self.snapshot()))

    def test_deleted_tracked_source_changes_digest_without_disabling(self):
        before = self.snapshot()
        self.source_file.unlink()
        after = self.snapshot()
        self.assertNotEqual(before.digest, after.digest)

    def test_source_inventory_excludes_custom_build_but_includes_untracked_files(self):
        def git(args, **kwargs):
            if args[1] == "rev-parse":
                return str(self.source).encode() + b"\n"
            return b"parser.cpp\0new_fixture.fl\0custom-build/.flecha-test-cache.json\0"
        # Invoke the actual method despite the dependency-isolation fixture.
        with patch.object(self.context, "_run", side_effect=git):
            paths = CacheContext._source_files(self.context)
        self.assertEqual(paths, [self.source / "new_fixture.fl", self.source_file])

    def test_empty_optional_submodule_is_fingerprinted(self):
        submodule = self.source / "third_party" / "optional"
        submodule.mkdir(parents=True)

        def git(args, **kwargs):
            if args[1] == "rev-parse":
                return str(self.source).encode() + b"\n"
            return b"parser.cpp\0third_party/optional\0"

        with patch.object(self.context, "_run", side_effect=git):
            paths = CacheContext._source_files(self.context)
        self.assertIn(submodule, paths)
        self.assertIsInstance(CacheContext._hash_path(submodule), str)
        (submodule / "new_file").write_text("initializing dependency")
        from test_cache import CacheUnavailable
        with self.assertRaises(CacheUnavailable):
            CacheContext._hash_path(submodule)

    def test_submodule_inventory_includes_dirty_and_untracked_working_tree(self):
        submodule = self.source / "dependency"
        submodule.mkdir()
        (submodule / "tracked.cpp").write_text("dirty source")
        (submodule / "untracked.h").write_text("new input")

        def git(args, **kwargs):
            if args[1] == "rev-parse":
                return str(self.source).encode() + b"\n"
            if args[1] == "-C":
                if "rev-parse" in args:
                    return str(submodule).encode() + b"\n"
                return b"tracked.cpp\0untracked.h\0"
            return b"parser.cpp\0dependency\0"

        with patch.object(self.context, "_run", side_effect=git):
            paths = CacheContext._source_files(self.context)
        self.assertEqual(paths, [submodule / "tracked.cpp", submodule / "untracked.h",
                                 self.source_file])

    def test_symlink_target_contents_and_mode_are_hashed(self):
        link = self.root / "link"
        link.symlink_to(self.library)
        before = CacheContext._hash_path(link)
        self.library.write_text("new target bytes")
        self.assertNotEqual(before, CacheContext._hash_path(link))
        before = CacheContext._hash_path(link)
        self.library.chmod(0o700)
        self.assertNotEqual(before, CacheContext._hash_path(link))


class RuntimeInventoryTests(unittest.TestCase):
    def test_missing_shared_library_is_not_silently_omitted(self):
        context = CacheContext(Path("/tmp"), Path("/tmp/build"))
        with patch.object(context, "_run", return_value=b"libfoo.so => not found\n"):
            from test_cache import CacheUnavailable
            with self.assertRaises(CacheUnavailable):
                context._shared_libraries(Path("/tmp/test_flecha"))

    def test_loader_and_transitive_libraries_are_collected(self):
        context = CacheContext(Path("/tmp"), Path("/tmp/build"))
        output = (b"linux-vdso.so.1 (0x1234)\n"
                  b"libfoo.so => /usr/lib/libfoo.so (0x1234)\n"
                  b"/lib64/ld-linux-x86-64.so.2 (0x4567)\n")
        with patch.object(context, "_run", return_value=output):
            self.assertEqual(context._shared_libraries(Path("/tmp/test_flecha")),
                             [Path("/usr/lib/libfoo.so"), Path("/lib64/ld-linux-x86-64.so.2")])


if __name__ == "__main__":
    unittest.main()
