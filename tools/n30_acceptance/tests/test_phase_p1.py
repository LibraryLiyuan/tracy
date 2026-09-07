import io
import tempfile
import unittest
from pathlib import Path

from tools.n30_acceptance.comparison_log import ComparisonRecorder
from tools.n30_acceptance.phase_p1 import evaluate_harness_selftest, parse_unittest_summary
from tools.n30_acceptance.status import AcceptanceStatus


class HarnessSelftestTests(unittest.TestCase):
    def test_parses_verbose_unittest_summary_from_stderr(self):
        self.assertEqual(
            parse_unittest_summary("...\nRan 57 tests in 0.901s\n\nOK\n"),
            {"tests": 57, "result": "OK"},
        )
        self.assertEqual(
            parse_unittest_summary("Ran 57 tests in 1.0s\nFAILED (failures=1)\n"),
            {"tests": 57, "result": "FAILED"},
        )

    def test_prints_exit_test_count_and_result_comparisons(self):
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=console)
            outcome = evaluate_harness_selftest(
                exit_code=0,
                stderr="Ran 57 tests in 0.5s\n\nOK\n",
                minimum_tests=50,
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.PASS)
            rendered = console.getvalue()
            self.assertIn("P1.unittest.exit_code", rendered)
            self.assertIn("P1.unittest.test_count", rendered)
            self.assertIn("P1.unittest.result", rendered)

    def test_failed_or_unparseable_selftest_is_harness_error(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_harness_selftest(
                exit_code=1,
                stderr="traceback without summary",
                minimum_tests=50,
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.HARNESS_ERROR)


if __name__ == "__main__":
    unittest.main()
