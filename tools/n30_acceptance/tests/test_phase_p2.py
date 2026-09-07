import io
import tempfile
import unittest
from pathlib import Path

from tools.n30_acceptance.comparison_log import ComparisonRecorder
from tools.n30_acceptance.phase_p2 import (
    ToolchainObservation,
    build_toolchain_commands,
    evaluate_toolchain,
    parse_ctest_summary,
)
from tools.n30_acceptance.status import AcceptanceStatus


class ToolchainPlanTests(unittest.TestCase):
    def test_plan_builds_fresh_capture_and_query_with_required_dev_oracles(self):
        commands = build_toolchain_commands(
            cmake=Path("C:/cmake.exe"),
            ctest=Path("C:/ctest.exe"),
            worktree=Path("C:/repo"),
            capture_build=Path("C:/repo/build-capture"),
            query_build=Path("C:/repo/build-query"),
            dependency_cache=Path("C:/deps"),
        )
        flattened = [item for command in commands for item in command.command]
        self.assertIn("-DJN_STREAM_CONVERT_DEV_TOOLS=ON", flattened)
        self.assertIn("tracy-stream-convert-legacy", flattened)
        self.assertIn("tracy-trace-compare", flattened)
        self.assertIn("tracy-query", flattened)
        self.assertIn("tracy-trace-session-inventory-tests", flattened)
        self.assertTrue(any(command.label == "P2-query-ctest" for command in commands))

    def test_ctest_summary_is_strict(self):
        self.assertEqual(
            parse_ctest_summary("100% tests passed, 0 tests failed out of 7\n"),
            {"passed_percent": 100, "failed": 0, "total": 7},
        )
        with self.assertRaises(ValueError):
            parse_ctest_summary("No tests were found!!!")


class ToolchainEvaluationTests(unittest.TestCase):
    def test_every_command_tool_and_schema_is_printed(self):
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=console)
            observation = ToolchainObservation(
                command_exit_codes={"configure": 0, "build": 0, "ctest": 0},
                required_tools={"converter": True, "query": True, "legacy": True},
                ctest={"passed_percent": 100, "failed": 0, "total": 7},
                query_schema="1.34.0",
                tool_sha256={"converter": "a" * 64, "query": "b" * 64},
            )
            outcome = evaluate_toolchain(
                observation,
                expected_query_schema="1.34.0",
                minimum_ctest_count=7,
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.PASS)
            rendered = console.getvalue()
            self.assertIn("P2.command.configure.exit_code", rendered)
            self.assertIn("P2.tool.converter.exists", rendered)
            self.assertIn("P2.query.schema", rendered)
            self.assertIn("P2.ctest.total", rendered)

    def test_any_build_or_test_failure_is_harness_error(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_toolchain(
                ToolchainObservation(
                    command_exit_codes={"build": 1},
                    required_tools={"query": False},
                    ctest={"passed_percent": 0, "failed": 1, "total": 1},
                    query_schema="missing",
                    tool_sha256={},
                ),
                expected_query_schema="1.34.0",
                minimum_ctest_count=7,
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.HARNESS_ERROR)


if __name__ == "__main__":
    unittest.main()
