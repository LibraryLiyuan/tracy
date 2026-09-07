import io
from pathlib import Path
import tempfile
import unittest

from tools.n30_acceptance.comparison_log import ComparisonRecorder
from tools.n30_acceptance.phase_p5 import evaluate_real_30m_metrics
from tools.n30_acceptance.status import AcceptanceStatus


class RealThirtyMinuteAuditTests(unittest.TestCase):
    def test_three_way_exact_comparison_is_printed_for_every_metric(self):
        golden = {"trace.counts.frames": "7", "gpu.verifier.report_hash": "11"}
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=console)
            outcome = evaluate_real_30m_metrics(
                golden_metrics=golden,
                fresh_metrics=dict(golden),
                existing_metrics=dict(golden),
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.PASS)
            rendered = console.getvalue()
            self.assertIn("P5.fresh_vs_golden.trace.counts.frames", rendered)
            self.assertIn("P5.existing_vs_golden.trace.counts.frames", rendered)
            self.assertIn("P5.fresh_vs_existing.trace.counts.frames", rendered)
            self.assertEqual(recorder.passed, 6)

    def test_missing_or_changed_metric_is_correctness_failure_without_early_exit(self):
        golden = {"one": "1", "two": "2"}
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_real_30m_metrics(
                golden_metrics=golden,
                fresh_metrics={"one": "9"},
                existing_metrics=dict(golden),
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_CORRECTNESS)
            self.assertEqual(len(outcome.evidence["failed_comparisons"]), 4)
            self.assertEqual(recorder.sequence, 6)


if __name__ == "__main__":
    unittest.main()
