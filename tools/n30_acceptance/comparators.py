"""Exact, machine-readable acceptance comparisons."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
import re
from typing import Any


_DECIMAL_INTEGER = re.compile(r"-?(?:0|[1-9][0-9]*)\Z")


class ComparisonStatus(str, Enum):
    PASS = "PASS"
    FAIL = "FAIL"


@dataclass(frozen=True)
class ComparisonResult:
    name: str
    status: ComparisonStatus
    expected: Any
    actual: Any
    delta: Any
    rule: str
    detail: str = ""

    def as_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "status": self.status.value,
            "expected": self.expected,
            "actual": self.actual,
            "delta": self.delta,
            "rule": self.rule,
            "detail": self.detail,
        }


def parse_exact_int(value: Any) -> int:
    """Parse an integer without permitting lossy JSON numeric coercion."""

    if isinstance(value, bool):
        raise TypeError("boolean is not an exact integer")
    if isinstance(value, int):
        return value
    if not isinstance(value, str):
        raise TypeError(f"expected integer or decimal string, got {type(value).__name__}")
    if _DECIMAL_INTEGER.fullmatch(value) is None:
        raise ValueError(f"not a canonical decimal integer: {value!r}")
    return int(value, 10)


def compare_exact(name: str, expected: Any, actual: Any) -> ComparisonResult:
    expected_int = parse_exact_int(expected)
    actual_int = parse_exact_int(actual)
    delta = actual_int - expected_int
    return ComparisonResult(
        name=name,
        status=ComparisonStatus.PASS if delta == 0 else ComparisonStatus.FAIL,
        expected=expected_int,
        actual=actual_int,
        delta=delta,
        rule="exact_integer",
    )


def compare_equal(name: str, expected: Any, actual: Any) -> ComparisonResult:
    equal = type(actual) is type(expected) and actual == expected
    return ComparisonResult(
        name=name,
        status=ComparisonStatus.PASS if equal else ComparisonStatus.FAIL,
        expected=expected,
        actual=actual,
        delta="equal" if equal else "actual != expected",
        rule="strict_equality",
    )


def compare_true(name: str, actual: Any) -> ComparisonResult:
    if not isinstance(actual, bool):
        raise TypeError(f"expected bool, got {type(actual).__name__}")
    return ComparisonResult(
        name=name,
        status=ComparisonStatus.PASS if actual else ComparisonStatus.FAIL,
        expected=True,
        actual=actual,
        delta="true" if actual else "false",
        rule="is_true",
    )


def compare_at_most(name: str, maximum: Any, actual: Any) -> ComparisonResult:
    maximum_int = parse_exact_int(maximum)
    actual_int = parse_exact_int(actual)
    margin = actual_int - maximum_int
    return ComparisonResult(
        name=name,
        status=ComparisonStatus.PASS if margin <= 0 else ComparisonStatus.FAIL,
        expected=maximum_int,
        actual=actual_int,
        delta=margin,
        rule="integer_at_most",
    )


def compare_at_least(name: str, minimum: Any, actual: Any) -> ComparisonResult:
    minimum_int = parse_exact_int(minimum)
    actual_int = parse_exact_int(actual)
    margin = actual_int - minimum_int
    return ComparisonResult(
        name=name,
        status=ComparisonStatus.PASS if margin >= 0 else ComparisonStatus.FAIL,
        expected=minimum_int,
        actual=actual_int,
        delta=margin,
        rule="integer_at_least",
    )


def compare_ratio(
    name: str,
    expected_numerator: Any,
    expected_denominator: Any,
    actual_numerator: Any,
    actual_denominator: Any,
) -> ComparisonResult:
    expected_num = parse_exact_int(expected_numerator)
    expected_den = parse_exact_int(expected_denominator)
    actual_num = parse_exact_int(actual_numerator)
    actual_den = parse_exact_int(actual_denominator)
    if expected_den <= 0 or actual_den <= 0:
        raise ValueError("ratio denominators must be positive")
    left = actual_num * expected_den
    right = expected_num * actual_den
    delta = left - right
    return ComparisonResult(
        name=name,
        status=ComparisonStatus.PASS if delta == 0 else ComparisonStatus.FAIL,
        expected={"numerator": expected_num, "denominator": expected_den},
        actual={"numerator": actual_num, "denominator": actual_den},
        delta=delta,
        rule="exact_rational_cross_product",
    )


# This list is deliberately fixed in source.  Profiles and command-line input
# cannot extend it, so an acceptance run cannot hide a semantic difference.
_EXCLUDED_PATHS = frozenset(
    {
        "run.started_at_utc",
        "run.duration_ns",
        "process.pid",
        "paths.temporary_directory",
        "metrics.wall_clock_ns",
        "report.generated_at_utc",
        "process.started_at_utc",
        "filesystem.enumeration_order",
        "logs.absolute_path",
    }
)


def is_excluded_path(path: str) -> bool:
    return path in _EXCLUDED_PATHS


def comparison_exclusions() -> tuple[str, ...]:
    return tuple(sorted(_EXCLUDED_PATHS))
