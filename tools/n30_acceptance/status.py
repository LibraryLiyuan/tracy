"""Acceptance phase and run status semantics."""

from __future__ import annotations

from enum import Enum
from typing import Iterable


class AcceptanceStatus(str, Enum):
    PASS = "PASS"
    FAIL_CORRECTNESS = "FAIL_CORRECTNESS"
    FAIL_INTEGRITY = "FAIL_INTEGRITY"
    FAIL_PERFORMANCE = "FAIL_PERFORMANCE"
    BLOCKED_ENVIRONMENT = "BLOCKED_ENVIRONMENT"
    HARNESS_ERROR = "HARNESS_ERROR"
    DEFERRED_OUT_OF_SCOPE = "DEFERRED_OUT_OF_SCOPE"


class OverallStatus(str, Enum):
    PASSED = "PASSED"
    FAILED = "FAILED"
    INCOMPLETE = "INCOMPLETE"


_FAILURES = frozenset(
    {
        AcceptanceStatus.FAIL_CORRECTNESS,
        AcceptanceStatus.FAIL_INTEGRITY,
        AcceptanceStatus.FAIL_PERFORMANCE,
    }
)
_INCOMPLETE = frozenset(
    {
        AcceptanceStatus.BLOCKED_ENVIRONMENT,
        AcceptanceStatus.HARNESS_ERROR,
    }
)


def aggregate_overall_status(statuses: Iterable[AcceptanceStatus]) -> OverallStatus:
    normalized = tuple(AcceptanceStatus(value) for value in statuses)
    if any(value in _FAILURES for value in normalized):
        return OverallStatus.FAILED
    if any(value in _INCOMPLETE for value in normalized):
        return OverallStatus.INCOMPLETE
    return OverallStatus.PASSED
