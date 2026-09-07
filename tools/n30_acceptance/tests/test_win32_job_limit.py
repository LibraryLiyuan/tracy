import sys
from pathlib import Path
import tempfile
import unittest

from tools.n30_acceptance.process_runner import ProcessRunner


@unittest.skipUnless(sys.platform == "win32", "Windows Job Object")
class JobLimitTests(unittest.TestCase):
    def test_child_cannot_allocate_beyond_explicit_job_limit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = ProcessRunner(sample_interval_seconds=0.05).run(
                [sys.executable, "-c", "import time; time.sleep(0.2); x=bytearray(512*1024*1024); print('UNEXPECTED_SUCCESS')"],
                cwd=root, stdout_path=root / "out", stderr_path=root / "err",
                metrics_path=root / "metrics", outer_job_bytes=128 * 1024**2,
                timeout_seconds=10, echo_output=False,
            )
            self.assertNotEqual(result.exit_code, 0)
            self.assertNotIn("UNEXPECTED_SUCCESS", (root / "out").read_text())
            self.assertLessEqual(result.peak_private_bytes, 128 * 1024**2)
