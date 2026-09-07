import io
from pathlib import Path
import tempfile
import unittest

from tools.n30_acceptance.comparison_log import ComparisonRecorder
from tools.n30_acceptance.phase_p4 import FreshConversionObservation, evaluate_fresh_conversion
from tools.n30_acceptance.status import AcceptanceStatus


class FreshConversionTests(unittest.TestCase):
    def _observation(self, **changes):
        values = dict(
            exit_code=0,
            timed_out=False,
            no_progress_timed_out=False,
            peak_private_bytes=100,
            session_exists=True,
            current_generation="n30-fresh",
            manifest={
                "generation": "n30-fresh",
                "store_schema": 1,
                "canonical_schema": 1,
                "derived_schema": 1,
                "source_protocol": 90,
                "source_sha256": "a" * 64,
                "source_size": 123,
                "state": 11,
                "mandatory_derived_complete": 1,
                "audit_complete": 1,
                "reason": "source_gap:7",
                "shard_count": 4,
            },
            summary={
                "session.canonical_shards": "4",
                "session.indexed_records": "99",
                "session.gpu_resources": "8",
                "session.gpu_passes": "9",
            },
        )
        values.update(changes)
        return FreshConversionObservation(**values)

    def _expected(self):
        return {
            "source_sha256": "a" * 64,
            "source_size": "123",
            "protocol": 90,
            "session_schema": 1,
            "canonical_schema": 1,
            "derived_schema": 1,
            "state_code": 11,
            "state": "CompleteSourceDegraded",
            "reason": "source_gap:7",
            "hard_private_bytes": 200,
        }

    def test_prints_every_conversion_integrity_and_performance_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=console)
            outcome = evaluate_fresh_conversion(
                self._observation(), expected=self._expected(), recorder=recorder
            )
            self.assertEqual(outcome.status, AcceptanceStatus.PASS)
            rendered = console.getvalue()
            self.assertIn("P4.converter.exit_code", rendered)
            self.assertIn("P4.session.source_sha256", rendered)
            self.assertIn("P4.session.audit_complete", rendered)
            self.assertIn("P4.converter.peak_private_bytes", rendered)

    def test_memory_limit_is_performance_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_fresh_conversion(
                self._observation(peak_private_bytes=201),
                expected=self._expected(),
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_PERFORMANCE)

    def test_published_manifest_mismatch_is_integrity_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            observation = self._observation()
            observation = FreshConversionObservation(
                **{**observation.__dict__, "manifest": {**observation.manifest, "audit_complete": 0}}
            )
            outcome = evaluate_fresh_conversion(
                observation, expected=self._expected(), recorder=recorder
            )
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_INTEGRITY)


if __name__ == "__main__":
    unittest.main()
