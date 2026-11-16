"""Progress rendering and child lifecycle tests without real child processes."""

import contextlib
import io
import os
from pathlib import Path
import subprocess
import signal
import tempfile
import unittest
from unittest import mock

from test_progress import run_logged


class Terminal(io.StringIO):
    def isatty(self):
        return True


class ProgressTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.path = Path(temporary.name) / "run.log"
        self.log = self.path.open("w")
        self.addCleanup(self.log.close)
        self.process = mock.Mock()
        self.process.pid = 12345
        self.process.wait.return_value = 0
        patch = mock.patch("test_progress.subprocess.Popen", return_value=self.process)
        self.popen = patch.start()
        self.addCleanup(patch.stop)
        patch = mock.patch("test_progress.os.killpg", create=True)
        self.killpg = patch.start()
        self.addCleanup(patch.stop)
        self.now = 0
        patch = mock.patch("test_progress.time.monotonic", side_effect=self.clock)
        patch.start()
        self.addCleanup(patch.stop)

    def clock(self):
        self.now += 5
        return self.now

    def append(self, text):
        self.log.write(text)
        self.log.flush()

    def test_nonterminal_reports_counts_from_partial_lines_and_preserves_log(self):
        self.append("99/99 Test #99: previous command\n")
        steps = iter(["1/2 Test #1: success\n2/", "2 Test #2: success\n", None])

        def wait(**kwargs):
            text = next(steps)
            if text is None:
                return 0
            self.append(text)
            raise subprocess.TimeoutExpired("ctest", 1)

        self.process.wait.side_effect = wait
        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            self.assertEqual(run_logged(["ctest"], log=self.log, label="Tests", total=2), 0)
        self.assertIn("Tests 0/2", output.getvalue())
        self.assertIn("Tests 1/2", output.getvalue())
        self.assertIn("Tests 2/2", output.getvalue())
        self.assertNotIn("99/2", output.getvalue())
        self.assertIn("done (exit 0)", output.getvalue())
        self.assertIn("previous command", self.path.read_text())
        self.popen.assert_called_once_with(["ctest"], stdout=self.log, stderr=subprocess.STDOUT,
                                          start_new_session=os.name != "nt")

    def test_terminal_bar_reads_final_output_even_without_timeout(self):
        def wait(**kwargs):
            self.append("1/1 Test #1: passed")
            return 0

        self.process.wait.side_effect = wait
        output = Terminal()
        with contextlib.redirect_stderr(output):
            run_logged(["ctest"], log=self.log, label="Tests", total=1)
        self.assertIn("\rTests [====================] 1/1", output.getvalue())
        self.assertTrue(output.getvalue().endswith("\n"))

    def test_build_has_loading_dots_without_fabricated_percentage(self):
        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            run_logged(["cmake"], log=self.log, label="Build")
        self.assertIn("Build .", output.getvalue())
        self.assertNotIn("[", output.getvalue())
        self.assertNotIn("/", output.getvalue())

    def test_quiet_mode_preserves_failure_exit_and_emits_nothing(self):
        self.process.wait.return_value = 8
        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            self.assertEqual(run_logged(["ctest"], log=self.log, label="Tests", quiet=True), 8)
        self.assertEqual(output.getvalue(), "")

    def test_interruption_terminates_and_reaps_child(self):
        self.process.wait.side_effect = [KeyboardInterrupt(), 0]
        with self.assertRaises(KeyboardInterrupt):
            run_logged(["ctest"], log=self.log, label="Tests", quiet=True)
        if os.name == "nt":
            self.process.terminate.assert_called_once_with()
        else:
            self.killpg.assert_called_once_with(12345, signal.SIGTERM)
        self.process.kill.assert_not_called()
        self.assertEqual(self.process.wait.call_count, 2)

    def test_unresponsive_interrupted_child_is_killed_and_reaped(self):
        self.process.wait.side_effect = [KeyboardInterrupt(), subprocess.TimeoutExpired("ctest", 5), -9]
        with self.assertRaises(KeyboardInterrupt):
            run_logged(["ctest"], log=self.log, label="Tests", quiet=True)
        if os.name == "nt":
            self.process.terminate.assert_called_once_with()
            self.process.kill.assert_called_once_with()
        else:
            self.assertEqual(self.killpg.call_args_list,
                             [mock.call(12345, signal.SIGTERM), mock.call(12345, signal.SIGKILL)])
        self.assertEqual(self.process.wait.call_count, 3)


if __name__ == "__main__":
    unittest.main()
