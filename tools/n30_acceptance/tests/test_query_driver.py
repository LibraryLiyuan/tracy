import json
from pathlib import Path
import tempfile
import unittest


from tools.n30_acceptance.query_driver import (
    QueryContractError,
    QueryTiming,
    execute_query_batch,
    load_ndjson_requests,
    maximum_ndjson_line_bytes,
    parse_ndjson_responses,
    summarize_timings,
)


def response(identifier, *, ok=True):
    base = {
        "protocol": "tracy-query/1",
        "schema_version": "1.34.0",
        "id": identifier,
        "ok": ok,
    }
    if ok:
        base.update(
            {
                "data": {"count": "1"},
                "partial": False,
                "omitted_count": "0",
                "budget": {
                    "limits": {},
                    "consumed": {},
                    "exhausted_by": [],
                    "omitted_count_exact": True,
                },
                "warnings": [],
            }
        )
    else:
        base["error"] = {
            "code": "INCOMPLETE_DATA",
            "message": "fixture",
            "retryable": False,
            "details": {},
        }
    return base


class RequestTests(unittest.TestCase):
    def test_loads_requests_and_rejects_duplicate_ids(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "requests.ndjson"
            path.write_text(
                "\n".join(
                    json.dumps(value)
                    for value in (
                        {"protocol": "tracy-query/1", "id": "a", "method": "trace.info", "params": {}},
                        {"protocol": "tracy-query/1", "id": "b", "method": "trace.counts", "params": {}},
                    )
                ),
                encoding="utf-8",
            )
            requests = load_ndjson_requests(path)
            self.assertEqual([item["id"] for item in requests], ["a", "b"])

            path.write_text(
                "\n".join(json.dumps({"protocol": "tracy-query/1", "id": "a", "method": "trace.info"}) for _ in range(2)),
                encoding="utf-8",
            )
            with self.assertRaises(QueryContractError):
                load_ndjson_requests(path)


class ResponseTests(unittest.TestCase):
    def test_reports_largest_nonempty_ndjson_response_line(self):
        self.assertEqual(maximum_ndjson_line_bytes(b"a\n\nlonger\r\nxx"), 6)

    def test_requires_one_response_per_expected_id_in_request_order(self):
        payload = "\n".join(json.dumps(response(value)) for value in ("b", "a"))
        parsed = parse_ndjson_responses(
            payload.encode("utf-8"), expected_ids=["a", "b"]
        )
        self.assertEqual([item["id"] for item in parsed], ["a", "b"])

    def test_rejects_missing_duplicate_unknown_and_wrong_schema(self):
        bad_cases = (
            (json.dumps(response("a")), ["a", "b"]),
            ("\n".join((json.dumps(response("a")), json.dumps(response("a")))), ["a"]),
            (json.dumps(response("other")), ["a"]),
            (json.dumps({**response("a"), "schema_version": "1.33.0"}), ["a"]),
        )
        for payload, expected in bad_cases:
            with self.subTest(payload=payload):
                with self.assertRaises(QueryContractError):
                    parse_ndjson_responses(
                        payload.encode("utf-8"),
                        expected_ids=expected,
                        expected_schema="1.34.0",
                    )

    def test_enforces_response_byte_limit(self):
        payload = json.dumps(response("a")).encode("utf-8")
        with self.assertRaises(QueryContractError):
            parse_ndjson_responses(
                payload,
                expected_ids=["a"],
                max_response_bytes=len(payload) - 1,
            )


class TimingTests(unittest.TestCase):
    def test_three_attempts_keep_each_value_and_report_median_and_worst(self):
        attempts = [
            QueryTiming(ordinal=1, elapsed_ns=5_000_000, peak_private_bytes=100),
            QueryTiming(ordinal=2, elapsed_ns=1_000_000, peak_private_bytes=300),
            QueryTiming(ordinal=3, elapsed_ns=3_000_000, peak_private_bytes=200),
        ]
        summary = summarize_timings(attempts, required_attempts=3)
        self.assertEqual(summary.median_elapsed_ns, 3_000_000)
        self.assertEqual(summary.worst_elapsed_ns, 5_000_000)
        self.assertEqual(summary.peak_private_bytes, 300)
        self.assertEqual(summary.attempts, tuple(attempts))

    def test_exactly_three_attempts_are_required_by_default(self):
        with self.assertRaises(QueryContractError):
            summarize_timings(
                [QueryTiming(ordinal=1, elapsed_ns=1, peak_private_bytes=1)]
            )


class _FakeProcessResult:
    exit_code = 0
    elapsed_ns = 1234
    peak_private_bytes = 5678


class _FakeRunner:
    def __init__(self, payload):
        self.payload = payload
        self.commands = []

    def run(self, command, **kwargs):
        self.commands.append((tuple(command), kwargs))
        output = Path(command[command.index("--output") + 1])
        output.write_bytes(self.payload)
        return _FakeProcessResult()


class ExecuteTests(unittest.TestCase):
    def test_executes_official_query_with_locked_batch_and_parses_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            requests = root / "requests.ndjson"
            requests.write_text(
                json.dumps(
                    {
                        "protocol": "tracy-query/1",
                        "id": "count",
                        "method": "trace.counts",
                        "params": {},
                    }
                ),
                encoding="utf-8",
            )
            runner = _FakeRunner(json.dumps(response("count")).encode("utf-8"))
            result = execute_query_batch(
                query_executable=root / "tracy-query.exe",
                trace_path=root / "trace.jn-trace-session",
                request_path=requests,
                allow_root=root,
                attempt_directory=root / "attempt-001",
                runner=runner,
                ordinal=1,
            )
            self.assertEqual(result.responses[0]["id"], "count")
            self.assertEqual(result.timing.elapsed_ns, 1234)
            command = runner.commands[0][0]
            self.assertIn("--batch", command)
            self.assertIn("--output", command)
            self.assertIn("--allow-root", command)

    def test_nonzero_query_exit_is_contract_failure(self):
        class FailedRunner(_FakeRunner):
            def run(self, command, **kwargs):
                result = super().run(command, **kwargs)
                result.exit_code = 9
                return result

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            requests = root / "requests.ndjson"
            requests.write_text(
                json.dumps(
                    {
                        "protocol": "tracy-query/1",
                        "id": "a",
                        "method": "trace.info",
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaises(QueryContractError):
                execute_query_batch(
                    query_executable=root / "tracy-query.exe",
                    trace_path=root / "trace.jn-trace-session",
                    request_path=requests,
                    allow_root=root,
                    attempt_directory=root / "attempt-001",
                    runner=FailedRunner(json.dumps(response("a")).encode()),
                    ordinal=1,
                )


if __name__ == "__main__":
    unittest.main()
