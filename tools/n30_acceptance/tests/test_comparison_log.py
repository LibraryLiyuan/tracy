import io
import json
from pathlib import Path
import tempfile
import unittest


from tools.n30_acceptance.comparison_log import ComparisonRecorder
from tools.n30_acceptance.comparators import compare_exact, compare_ratio


class ComparisonRecorderTests(unittest.TestCase):
    def test_every_comparison_is_printed_and_persisted(self):
        console = io.StringIO()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "comparisons.ndjson"
            recorder = ComparisonRecorder(output, console=console)
            recorder.record(compare_exact("gpu.resources", "7", "7"))
            recorder.record(compare_exact("gpu.passes", "9", "8"))

            lines = console.getvalue().splitlines()
            self.assertEqual(len(lines), 2)
            self.assertEqual(
                lines[0],
                "[COMPARE] name=gpu.resources | rule=exact_integer | expected=7 | actual=7 | delta=0 | status=PASS",
            )
            self.assertEqual(
                lines[1],
                "[COMPARE] name=gpu.passes | rule=exact_integer | expected=9 | actual=8 | delta=-1 | status=FAIL",
            )

            records = [json.loads(line) for line in output.read_text(encoding="utf-8").splitlines()]
            self.assertEqual([record["sequence"] for record in records], [1, 2])
            self.assertEqual([record["status"] for record in records], ["PASS", "FAIL"])

    def test_ratio_log_preserves_numerator_and_denominator(self):
        console = io.StringIO()
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(
                Path(directory) / "comparisons.ndjson", console=console
            )
            recorder.record(compare_ratio("coverage", 1, 2, 2, 4))
            rendered = console.getvalue()
            self.assertIn("expected=1/2", rendered)
            self.assertIn("actual=2/4", rendered)
            self.assertIn("status=PASS", rendered)

    def test_summary_is_printed_with_pass_and_fail_counts(self):
        console = io.StringIO()
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(
                Path(directory) / "comparisons.ndjson", console=console
            )
            recorder.record(compare_exact("one", 1, 1))
            recorder.record(compare_exact("two", 2, 3))
            summary = recorder.print_summary("P5")
            self.assertEqual(summary, {"total": 2, "passed": 1, "failed": 1})
            self.assertIn(
                "[COMPARE-SUMMARY][P5] total=2 passed=1 failed=1",
                console.getvalue(),
            )

    def test_phase_summary_can_report_only_comparisons_since_checkpoint(self):
        console = io.StringIO()
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(
                Path(directory) / "comparisons.ndjson", console=console
            )
            recorder.record(compare_exact("P0.first", 1, 1))
            checkpoint = recorder.checkpoint()
            recorder.record(compare_exact("P1.pass", 2, 2))
            recorder.record(compare_exact("P1.fail", 3, 4))
            summary = recorder.print_summary("P1", since=checkpoint)
            self.assertEqual(summary, {"total": 2, "passed": 1, "failed": 1})
            self.assertIn(
                "[COMPARE-SUMMARY][P1] total=2 passed=1 failed=1",
                console.getvalue(),
            )

    def test_reopening_existing_log_continues_sequence_and_counts(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "comparisons.ndjson"
            first = ComparisonRecorder(output, console=io.StringIO())
            first.record(compare_exact("first.pass", 1, 1))
            first.record(compare_exact("first.fail", 1, 2))

            second = ComparisonRecorder(output, console=io.StringIO())
            self.assertEqual(second.checkpoint(), (1, 1))
            second.record(compare_exact("second.pass", 2, 2))
            records = [json.loads(line) for line in output.read_text().splitlines()]
            self.assertEqual([record["sequence"] for record in records], [1, 2, 3])


if __name__ == "__main__":
    unittest.main()
