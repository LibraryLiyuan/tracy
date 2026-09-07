"""P1 acceptance-harness self-test gate."""

from __future__ import annotations

import re
from typing import Any

from .acceptance_runner import PhaseOutcome
from .comparison_log import ComparisonRecorder
from .comparators import (
    ComparisonStatus,
    compare_at_least,
    compare_equal,
    compare_exact,
)
from .status import AcceptanceStatus


_RAN = re.compile(r"^Ran ([0-9]+) tests? in [^\r\n]+$", re.MULTILINE)
_RESULT = re.compile(r"^(OK(?: \([^\r\n]+\))?|FAILED(?: \([^\r\n]+\))?)$", re.MULTILINE)


def parse_unittest_summary(output: str) -> dict[str, Any]:
    ran = list(_RAN.finditer(output))
    result = list(_RESULT.finditer(output))
    if len(ran) != 1 or len(result) != 1:
        return {"tests": 0, "result": "UNPARSEABLE"}
    result_name = "OK" if result[0].group(1).startswith("OK") else "FAILED"
    return {"tests": int(ran[0].group(1), 10), "result": result_name}


def evaluate_harness_selftest(
    *,
    exit_code: int,
    stderr: str,
    minimum_tests: int,
    recorder: ComparisonRecorder,
) -> PhaseOutcome:
    parsed = parse_unittest_summary(stderr)
    checks = (
        compare_exact("P1.unittest.exit_code", 0, exit_code),
        compare_at_least("P1.unittest.test_count", minimum_tests, parsed["tests"]),
        compare_equal("P1.unittest.result", "OK", parsed["result"]),
    )
    for check in checks:
        recorder.record(check)
    failures = [check.name for check in checks if check.status == ComparisonStatus.FAIL]
    if failures:
        return PhaseOutcome(
            "P1",
            AcceptanceStatus.HARNESS_ERROR,
            f"acceptance harness self-test failed ({len(failures)} comparisons)",
            repair_direction="repair the Python harness before invoking N30 tools",
            evidence={"failed_comparisons": failures, "parsed": parsed},
        )
    return PhaseOutcome(
        "P1",
        AcceptanceStatus.PASS,
        f"Python acceptance harness passed {parsed['tests']} tests",
        evidence={"parsed": parsed},
    )


def run_p1(context) -> PhaseOutcome:
    harness = context.profile.get("harness")
    if not isinstance(harness, dict):
        raise ValueError("profile is missing harness policy")
    result, stdout, stderr = context.commands.run_text(
        "P1-python-harness-selftest",
        (
            str(context.inputs.python_executable),
            "-m",
            "unittest",
            "discover",
            "-s",
            "tools\\n30_acceptance\\tests",
            "-p",
            "test_*.py",
            "-v",
        ),
        cwd=context.inputs.worktree,
        timeout_seconds=300,
    )
    return evaluate_harness_selftest(
        exit_code=result.exit_code,
        stderr=stderr + "\n" + stdout,
        minimum_tests=int(harness["minimum_unit_tests"]),
        recorder=context.recorder,
    )
