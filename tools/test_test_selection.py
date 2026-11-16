"""Policy and real-Git regression tests for local test selection."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from test_selection import SelectionError, changed_files, select_tests


class SelectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="test selection ")
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)
        self.tests = [
            {"name": name, "command": ["test_flecha"]}
            for name in ["ParserTests.Basic", "SemanticTests.Basic", "AnalyzerClientTests.Basic",
                         "LspProtocolTests.Basic", "CliTests.LspStdio", "LongbowCacheTests.Basic",
                         "StandardLibraryTests.FlechaModules", "DocumentationExampleTests.CheckBuildRun",
                         "EditorTests.VSCodeManifest", "BuildDiagnostics.ToolchainConsistency"]
        ]

    def write(self, path, contents):
        target = self.repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(contents)

    def selected(self, *paths):
        tests, reasons = select_tests(self.repo, self.tests, list(paths))
        self.assertTrue(reasons)
        return [test["name"] for test in tests]

    def test_multiline_cpp_suites_union_with_docs_preserves_discovery_order(self):
        self.write("src/tests/test_parser.cpp", "TEST_F(\n ParserTests,\n Basic) {}")
        self.assertEqual(self.selected("docs/testing.md", "src/tests/test_parser.cpp"),
                         ["ParserTests.Basic", "DocumentationExampleTests.CheckBuildRun"])

    def test_parameterized_suite_names(self):
        self.tests.append({"name": "Inputs/Parameterized.Values/0", "command": []})
        self.write("src/tests/test_example.cpp", "TEST_P(Parameterized, Values) {}")
        self.assertEqual(self.selected("src/tests/test_example.cpp"), ["Inputs/Parameterized.Values/0"])

    def test_unknown_shared_deleted_and_missing_suite_fall_back(self):
        for path in ["src/core/parser.cpp", "std/src/text/strings.fl", "src/tests/test_missing.cpp",
                     "src/tests/test_main.cpp", "CMakeLists.txt", "editors/neovim/init.vim",
                     "src/tests/fixtures/example.fl", "unmapped.file", "../outside.cpp"]:
            with self.subTest(path=path):
                self.assertEqual(len(self.selected(path)), len(self.tests))
        self.write("src/tests/test_new.cpp", "TEST(NewSuite, NewTest) {}")
        self.assertEqual(len(self.selected("src/tests/test_new.cpp")), len(self.tests))

    def test_unknown_change_overrides_focused_selection(self):
        self.assertEqual(len(self.selected("docs/testing.md", "src/semantic/semantic.cpp")), len(self.tests))

    def test_exact_helper_match_handles_paths_with_spaces(self):
        helper = self.repo / "src/tests/verify_a.cmake"
        self.tests.extend([
            {"name": "Native.A", "command": ["cmake", "-P", str(helper)]},
            {"name": "Native.B", "command": ["cmake", "-P", str(helper) + ".other"]},
        ])
        self.assertEqual(self.selected("src/tests/verify_a.cmake"), ["Native.A"])
        self.assertEqual(len(self.selected("src/tests/verify_unknown.cmake")), len(self.tests))

    def test_lsp_selects_analyzer_and_lsp_dependents(self):
        self.assertEqual(self.selected("src/include/lsp/protocol.hpp"),
                         ["AnalyzerClientTests.Basic", "LspProtocolTests.Basic", "CliTests.LspStdio"])

    def test_longbow_finds_non_longbow_named_integration_command(self):
        self.tests.append({"name": "NativeExternTests.CAbiAndLinking", "command": ["cmake", "-DLONGBOW=/build/longbow"]})
        self.assertEqual(self.selected("src/longbow/main.cpp"),
                         ["LongbowCacheTests.Basic", "StandardLibraryTests.FlechaModules", "NativeExternTests.CAbiAndLinking"])

    def test_empty_changes_are_explicit(self):
        self.assertEqual(self.selected(), [])

    def test_missing_focused_tests_fall_back(self):
        self.tests = self.tests[:2]
        self.assertEqual(len(self.selected("docs/testing.md")), len(self.tests))

    def test_documentation_inputs_include_their_direct_consumers(self):
        self.assertEqual(self.selected("docs/editors.md"),
                         ["DocumentationExampleTests.CheckBuildRun", "EditorTests.VSCodeManifest"])
        for path in ["docs/toolchains.md", "README.md"]:
            with self.subTest(path=path):
                self.assertEqual(self.selected(path),
                                 ["DocumentationExampleTests.CheckBuildRun", "BuildDiagnostics.ToolchainConsistency"])

    def test_missing_any_documentation_consumer_falls_back_to_full(self):
        for path, missing in [("docs/editors.md", "EditorTests.VSCodeManifest"),
                              ("docs/toolchains.md", "BuildDiagnostics.ToolchainConsistency"),
                              ("README.md", "BuildDiagnostics.ToolchainConsistency")]:
            with self.subTest(path=path):
                inventory = [test for test in self.tests if test["name"] != missing]
                selected, reasons = select_tests(self.repo, inventory, [path])
                self.assertEqual(selected, inventory)
                self.assertIn("missing in discovery", reasons[-1])


class ChangedFilesTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)
        self.git("init", "-b", "master")
        self.git("config", "user.name", "Test")
        self.git("config", "user.email", "test@example.invalid")
        for name in ["original.cpp", "staged.cpp", "unstaged.cpp", "deleted.cpp"]:
            (self.repo / name).write_text("original\n")
        self.git("add", ".")
        self.git("commit", "-qm", "base")
        self.git("checkout", "-qb", "feature")

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.repo), *args], check=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout

    def test_branch_dirty_untracked_rename_and_delete_are_unioned(self):
        self.git("mv", "original.cpp", "renamed.cpp")
        self.git("commit", "-qm", "rename")
        (self.repo / "staged.cpp").write_text("staged\n")
        self.git("add", "staged.cpp")
        (self.repo / "unstaged.cpp").write_text("unstaged\n")
        (self.repo / "deleted.cpp").unlink()
        (self.repo / "new file\nwith newline.cpp").write_text("new\n")
        self.assertEqual(changed_files(self.repo), sorted([
            "original.cpp", "renamed.cpp", "staged.cpp", "unstaged.cpp", "deleted.cpp", "new file\nwith newline.cpp",
        ]))

    def test_master_advancing_does_not_turn_master_only_changes_into_branch_changes(self):
        self.git("checkout", "-q", "master")
        (self.repo / "master-only.cpp").write_text("master\n")
        self.git("add", ".")
        self.git("commit", "-qm", "master advances")
        self.git("checkout", "-q", "feature")
        self.assertEqual(changed_files(self.repo), [])

    def test_bad_ref_raises_instead_of_returning_no_changes(self):
        with self.assertRaises(SelectionError):
            changed_files(self.repo, "missing-ref")


if __name__ == "__main__":
    unittest.main()
