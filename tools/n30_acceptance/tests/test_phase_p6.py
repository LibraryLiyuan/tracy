import io
from pathlib import Path
import tempfile
import unittest

from tools.n30_acceptance.comparison_log import ComparisonRecorder
from tools.n30_acceptance.phase_p6 import (
    CancellationObservation,
    PaginationObservation,
    QueryAttemptObservation,
    QueryProbeObservation,
    evaluate_cancellation,
    evaluate_pagination,
    evaluate_probe_attempts,
    evaluate_query_matrix,
    extract_page,
    open_mcp_session,
    require_tool_data,
)
from tools.n30_acceptance.status import AcceptanceStatus


class QueryMatrixEvaluationTests(unittest.TestCase):
    def _attempt(self, ordinal: int, *, hash_value: str = "same", elapsed_ns: int = 10):
        return QueryAttemptObservation(
            ordinal=ordinal,
            response_hashes={"overview": hash_value, "gpu-resource": hash_value},
            response_count=2,
            expected_response_count=2,
            max_response_bytes=1024,
            peak_private_bytes=2048,
            elapsed_ns=elapsed_ns,
            exit_code=0,
        )

    def test_three_attempts_print_each_contract_and_semantic_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=console)
            outcome = evaluate_query_matrix(
                [self._attempt(1), self._attempt(2), self._attempt(3)],
                recorder=recorder,
                required_attempts=3,
                max_response_bytes=16 * 1024 * 1024,
                hard_private_bytes=16 * 1024 * 1024 * 1024,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.PASS)
            rendered = console.getvalue()
            self.assertIn("P6.batch.attempt_1.response_count", rendered)
            self.assertIn("P6.batch.attempt_3.overview.semantic_hash", rendered)
            self.assertIn("expected=", rendered)
            self.assertIn("actual=", rendered)
            self.assertIn("delta=", rendered)

    def test_semantic_drift_or_missing_attempt_is_correctness_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_query_matrix(
                [self._attempt(1), self._attempt(2, hash_value="changed")],
                recorder=recorder,
                required_attempts=3,
                max_response_bytes=16 * 1024 * 1024,
                hard_private_bytes=16 * 1024 * 1024 * 1024,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_CORRECTNESS)


class PaginationEvaluationTests(unittest.TestCase):
    def test_extract_page_reads_refs_and_cursor_from_real_query_shape(self):
        structured = {
            "ok": True,
            "data": {"images": [{"ref": "a"}, {"ref": "b"}]},
            "page": {"next_cursor": "next"},
        }
        self.assertEqual(extract_page(structured, "images"), (("a", "b"), "next"))

    def test_tool_data_rejects_query_failure_instead_of_treating_it_as_empty(self):
        with self.assertRaisesRegex(ValueError, "INCOMPLETE_DATA"):
            require_tool_data({"ok": False, "error": {"code": "INCOMPLETE_DATA"}})

    def test_three_small_pages_must_equal_one_large_page_without_duplicates(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_pagination(
                PaginationObservation(
                    large_page_refs=("a", "b", "c", "d", "e", "f"),
                    small_page_refs=(("a", "b"), ("c", "d"), ("e", "f")),
                    cursors=("cursor-1", "cursor-2", None),
                ),
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.PASS)

    def test_duplicate_or_omitted_ref_is_correctness_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_pagination(
                PaginationObservation(
                    large_page_refs=("a", "b", "c", "d", "e", "f"),
                    small_page_refs=(("a", "b"), ("c", "c"), ("e",)),
                    cursors=("cursor-1", "cursor-2", None),
                ),
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_CORRECTNESS)


class _ReadyClient:
    def __init__(self):
        self.calls = []

    def call_tool(self, name, arguments, *, timeout_seconds):
        self.calls.append((name, arguments, timeout_seconds))
        if name == "tracy_trace_open":
            return {"ok": True, "data": {"trace_id": "trace-1"}}
        if name == "tracy_trace_status":
            return {"ok": True, "data": {"status": {"state": "ready"}}}
        raise AssertionError(name)


class McpSessionContractTests(unittest.TestCase):
    def test_open_returns_trace_id_only_after_ready_status(self):
        client = _ReadyClient()
        trace_id, elapsed_ns = open_mcp_session(client, Path("capture.jn-trace-session"), timeout_seconds=1)
        self.assertEqual(trace_id, "trace-1")
        self.assertGreaterEqual(elapsed_ns, 0)
        self.assertEqual([value[0] for value in client.calls], ["tracy_trace_open", "tracy_trace_status"])


class QueryProbeEvaluationTests(unittest.TestCase):
    def test_regular_latency_uses_three_attempt_median_but_logs_every_attempt(self):
        attempts = [
            QueryProbeObservation(1, 3_000_000_000, 1_000_000_000, 100, 1000, "same", True),
            QueryProbeObservation(2, 1_000_000_000, 1_500_000_000, 100, 1000, "same", True),
            QueryProbeObservation(3, 2_000_000_000, 1_100_000_000, 100, 1000, "same", True),
        ]
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=console)
            outcome = evaluate_probe_attempts(
                "overview",
                attempts,
                recorder=recorder,
                required_attempts=3,
                query_limit_ns=2_000_000_000,
                open_limit_ns=2_000_000_000,
                response_limit_bytes=16 * 1024 * 1024,
                hard_private_bytes=16 * 1024 * 1024 * 1024,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.PASS)
            rendered = console.getvalue()
            self.assertIn("P6.probe.overview.attempt_1.query_elapsed_ns.measurement", rendered)
            self.assertIn("P6.probe.overview.query_elapsed_ns.median_gate", rendered)

    def test_probe_semantic_drift_is_correctness_failure(self):
        attempts = [
            QueryProbeObservation(1, 1, 1, 1, 1, "a", True),
            QueryProbeObservation(2, 1, 1, 1, 1, "b", True),
            QueryProbeObservation(3, 1, 1, 1, 1, "a", True),
        ]
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_probe_attempts(
                "resource",
                attempts,
                recorder=recorder,
                required_attempts=3,
                query_limit_ns=2,
                open_limit_ns=2,
                response_limit_bytes=100,
                hard_private_bytes=100,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_CORRECTNESS)


class CancellationEvaluationTests(unittest.TestCase):
    def test_all_three_cancellations_must_finish_cancelled_within_two_seconds(self):
        attempts = [
            CancellationObservation(1, 1_000_000_000, "cancelled", True, 10),
            CancellationObservation(2, 1_500_000_000, "cancelled", True, 11),
            CancellationObservation(3, 2_000_000_000, "cancelled", True, 12),
        ]
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_cancellation(
                attempts,
                recorder=recorder,
                required_attempts=3,
                worst_limit_ns=2_000_000_000,
                hard_private_bytes=100,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.PASS)

    def test_completed_instead_of_cancelled_is_correctness_failure(self):
        attempts = [
            CancellationObservation(1, 1, "cancelled", True, 10),
            CancellationObservation(2, 1, "completed", True, 10),
            CancellationObservation(3, 1, "cancelled", True, 10),
        ]
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_cancellation(
                attempts,
                recorder=recorder,
                required_attempts=3,
                worst_limit_ns=2,
                hard_private_bytes=100,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_CORRECTNESS)


if __name__ == "__main__":
    unittest.main()
