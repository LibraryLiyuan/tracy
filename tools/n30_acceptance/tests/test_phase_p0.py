import io
import tempfile
import unittest
from pathlib import Path

from tools.n30_acceptance.comparison_log import ComparisonRecorder
from tools.n30_acceptance.identity_phase import GitPorcelainIdentity
from tools.n30_acceptance.phase_p0 import P0Snapshot, evaluate_p0_snapshot
from tools.n30_acceptance.status import AcceptanceStatus


class P0EvaluationTests(unittest.TestCase):
    def _snapshot(self):
        return P0Snapshot(
            git=GitPorcelainIdentity(
                head="new-head",
                branch="feature/JNTracy-N30-LongTraceSessionStore",
                upstream="origin/feature/JNTracy-N30-LongTraceSessionStore",
                ahead=2,
                behind=0,
                tracked_dirty=(),
                untracked=("build-n30-stream-tests/",),
            ),
            baseline_is_ancestor=True,
            source_size=100,
            source_sha256="a" * 64,
            short_stream_exists=True,
            short_trace_exists=True,
            current_generation="n30-a7",
            manifest={
                "generation": "n30-a7",
                "store_schema": 1,
                "canonical_schema": 1,
                "derived_schema": 1,
                "source_sha256": "a" * 64,
                "source_size": 100,
                "source_protocol": 90,
                "state": 11,
                "mandatory_derived_complete": 1,
                "audit_complete": 1,
                "reason": "source_gap:7",
                "shard_count": 402,
            },
            volume_capacity_bytes=1000,
            volume_free_bytes=800,
        )

    def test_all_environment_and_session_checks_are_printed_and_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=console)
            outcome = evaluate_p0_snapshot(
                self._snapshot(),
                expected={
                    "branch": "feature/JNTracy-N30-LongTraceSessionStore",
                    "source_size": "100",
                    "source_sha256": "a" * 64,
                    "protocol": 90,
                    "session_schema": 1,
                    "canonical_schema": 1,
                    "derived_schema": 1,
                    "session_state_code": 11,
                    "session_state": "CompleteSourceDegraded",
                    "degradation_reason": "source_gap:7",
                    "canonical_shards": "402",
                    "scratch_hard_bytes": 100,
                    "minimum_reserve_bytes": 100,
                    "minimum_reserve_percent": 10,
                },
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.PASS)
            self.assertGreaterEqual(recorder.passed, 17)
            rendered = console.getvalue()
            self.assertIn("P0.git.branch", rendered)
            self.assertIn("P0.source.sha256", rendered)
            self.assertIn("P0.session.audit_complete", rendered)
            self.assertIn("P0.disk.free_bytes", rendered)

    def test_any_identity_mismatch_blocks_the_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            snapshot = self._snapshot()
            snapshot = P0Snapshot(**{**snapshot.__dict__, "source_sha256": "b" * 64})
            outcome = evaluate_p0_snapshot(
                snapshot,
                expected={
                    "branch": "feature/JNTracy-N30-LongTraceSessionStore",
                    "source_size": "100",
                    "source_sha256": "a" * 64,
                    "protocol": 90,
                    "session_schema": 1,
                    "canonical_schema": 1,
                    "derived_schema": 1,
                    "session_state_code": 11,
                    "session_state": "CompleteSourceDegraded",
                    "degradation_reason": "source_gap:7",
                    "canonical_shards": "402",
                    "scratch_hard_bytes": 100,
                    "minimum_reserve_bytes": 100,
                    "minimum_reserve_percent": 10,
                },
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.BLOCKED_ENVIRONMENT)
            self.assertIn("identity or capacity gate failed", outcome.summary)


if __name__ == "__main__":
    unittest.main()
