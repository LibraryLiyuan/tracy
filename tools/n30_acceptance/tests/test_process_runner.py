import json
import os
from pathlib import Path
import sys
import tempfile
import unittest


from tools.n30_acceptance.process_runner import ConsoleBreakPlan, ProcessRunner
from tools.n30_acceptance.win32_metrics import (
    capture_process_identity,
    sample_process_tree,
)


@unittest.skipUnless(sys.platform == "win32", "Windows process contract")
class Win32IdentityTests(unittest.TestCase):
    def test_current_process_identity_has_creation_time_and_exact_image(self):
        identity = capture_process_identity(os.getpid())
        self.assertEqual(
            Path(identity.image_path).resolve(), Path(sys.executable).resolve()
        )
        self.assertGreater(identity.creation_time_100ns, 0)

    def test_process_tree_sample_includes_root_and_resources(self):
        sample = sample_process_tree(os.getpid())
        self.assertIn(os.getpid(), sample.pids)
        self.assertGreater(sample.working_set_bytes, 0)
        self.assertGreater(sample.private_bytes, 0)
        self.assertGreaterEqual(sample.thread_count, 1)
        self.assertGreaterEqual(sample.handle_count, 1)


@unittest.skipUnless(sys.platform == "win32", "Windows process contract")
class ProcessRunnerTests(unittest.TestCase):
    def test_sends_verified_console_break_after_observing_target_stage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = (
                "import ctypes,time\n"
                "k=ctypes.WinDLL('kernel32',use_last_error=True)\n"
                "Handler=ctypes.WINFUNCTYPE(ctypes.c_int,ctypes.c_uint)\n"
                "@Handler\n"
                "def stop(kind):\n"
                " print('CANCELLED', flush=True)\n"
                " k.ExitProcess(130)\n"
                " return 1\n"
                "k.SetConsoleCtrlHandler(stop,True)\n"
                "print('TARGET_STAGE', flush=True)\n"
                "time.sleep(10)\n"
            )
            result = ProcessRunner(sample_interval_seconds=0.02).run(
                [sys.executable, "-c", script],
                cwd=root,
                stdout_path=root / "stdout.log",
                stderr_path=root / "stderr.log",
                metrics_path=root / "metrics.ndjson",
                timeout_seconds=5,
                console_break_plan=ConsoleBreakPlan("TARGET_STAGE"),
            )
            self.assertEqual(result.exit_code, 130)
            self.assertEqual(result.console_break_count, 1)
            self.assertIn("CANCELLED", (root / "stdout.log").read_text())

    def test_captures_stdout_stderr_exit_and_metrics(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runner = ProcessRunner(sample_interval_seconds=0.05)
            result = runner.run(
                [
                    sys.executable,
                    "-c",
                    (
                        "import sys,time; "
                        "print('out-line', flush=True); "
                        "print('err-line', file=sys.stderr, flush=True); "
                        "time.sleep(0.15)"
                    ),
                ],
                cwd=root,
                stdout_path=root / "stdout.log",
                stderr_path=root / "stderr.log",
                metrics_path=root / "metrics.ndjson",
                timeout_seconds=5,
            )
            self.assertEqual(result.exit_code, 0)
            self.assertIn("out-line", (root / "stdout.log").read_text())
            self.assertIn("err-line", (root / "stderr.log").read_text())
            metrics = [
                json.loads(line)
                for line in (root / "metrics.ndjson").read_text().splitlines()
            ]
            self.assertGreaterEqual(len(metrics), 1)
            self.assertGreater(result.peak_private_bytes, 0)
            self.assertFalse(result.timed_out)

    def test_nonzero_exit_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = ProcessRunner(sample_interval_seconds=0.02).run(
                [sys.executable, "-c", "raise SystemExit(17)"],
                cwd=root,
                stdout_path=root / "stdout.log",
                stderr_path=root / "stderr.log",
                metrics_path=root / "metrics.ndjson",
                timeout_seconds=5,
            )
            self.assertEqual(result.exit_code, 17)
            self.assertFalse(result.timed_out)

    def test_no_progress_timeout_is_distinct_and_stops_verified_process(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = ProcessRunner(sample_interval_seconds=0.02).run(
                [sys.executable, "-c", "import time; time.sleep(10)"],
                cwd=root,
                stdout_path=root / "stdout.log",
                stderr_path=root / "stderr.log",
                metrics_path=root / "metrics.ndjson",
                timeout_seconds=5,
                no_progress_timeout_seconds=0.15,
            )
            self.assertTrue(result.no_progress_timed_out)
            self.assertFalse(result.wall_clock_timed_out)
            self.assertLess(result.elapsed_ns, 2_000_000_000)


if __name__ == "__main__":
    unittest.main()
