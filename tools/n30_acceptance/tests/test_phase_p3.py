import io
import tempfile
import unittest
from pathlib import Path

from tools.n30_acceptance.comparison_log import ComparisonRecorder
from tools.n30_acceptance.phase_p3 import evaluate_short_differential
from tools.n30_acceptance.status import AcceptanceStatus


def response(identifier, data, *, ok=True):
    value = {
        "protocol": "tracy-query/1",
        "schema_version": "1.34.0",
        "id": identifier,
        "ok": ok,
    }
    if ok:
        value.update({"data": data, "partial": False, "omitted_count": "0"})
    else:
        value["error"] = {"code": "CAPABILITY_UNAVAILABLE", "details": {"domain": identifier}}
    return value


class ShortDifferentialTests(unittest.TestCase):
    def _responses(self, count="7"):
        return [
            response("counts", {"cpu_zones": count, "gpu_zones": "3"}),
            response("samples", {}, ok=False),
        ]

    def test_equal_worker_and_session_print_request_and_metric_comparisons(self):
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=console)
            outcome = evaluate_short_differential(
                self._responses(), self._responses(), recorder=recorder
            )
            self.assertEqual(outcome.status, AcceptanceStatus.PASS)
            rendered = console.getvalue()
            self.assertIn("P3.response.counts.ok", rendered)
            self.assertIn("P3.response.samples.error_code", rendered)
            self.assertIn("P3.metric.trace.counts.cpu_zones", rendered)
            self.assertIn("P3.response.counts.semantic_hash", rendered)

    def test_any_count_or_semantic_change_is_correctness_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_short_differential(
                self._responses("7"), self._responses("8"), recorder=recorder
            )
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_CORRECTNESS)
            self.assertGreater(len(outcome.evidence["failed_comparisons"]), 0)


if __name__ == "__main__":
    unittest.main()
