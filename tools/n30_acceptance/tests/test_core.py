import unittest


from tools.n30_acceptance.comparators import (
    ComparisonStatus,
    compare_at_least,
    compare_at_most,
    compare_exact,
    compare_equal,
    compare_ratio,
    compare_true,
    is_excluded_path,
    parse_exact_int,
)
from tools.n30_acceptance.status import (
    AcceptanceStatus,
    OverallStatus,
    aggregate_overall_status,
)


class ExactIntegerTests(unittest.TestCase):
    def test_accepts_arbitrary_precision_integer_and_decimal_string(self):
        huge = 2**100 + 123456789
        self.assertEqual(parse_exact_int(huge), huge)
        self.assertEqual(parse_exact_int(str(huge)), huge)
        self.assertEqual(parse_exact_int("-42"), -42)

    def test_rejects_bool_float_and_non_decimal_forms(self):
        for value in (True, False, 1.0, 1.5, "1.0", "1e3", "+1", " 1", "1 "):
            with self.subTest(value=value):
                with self.assertRaises((TypeError, ValueError)):
                    parse_exact_int(value)

    def test_exact_comparison_reports_machine_readable_delta(self):
        result = compare_exact("gpu.resources", "1769168", 1769167)
        self.assertEqual(result.status, ComparisonStatus.FAIL)
        self.assertEqual(result.expected, 1769168)
        self.assertEqual(result.actual, 1769167)
        self.assertEqual(result.delta, -1)
        self.assertEqual(result.rule, "exact_integer")


class GeneralComparisonTests(unittest.TestCase):
    def test_equal_preserves_human_readable_values(self):
        passed = compare_equal("git.branch", "feature/N30", "feature/N30")
        failed = compare_equal("git.branch", "feature/N30", "dev")
        self.assertEqual(passed.status, ComparisonStatus.PASS)
        self.assertEqual(failed.status, ComparisonStatus.FAIL)
        self.assertEqual(failed.delta, "actual != expected")

    def test_boolean_comparison_does_not_coerce_numbers(self):
        self.assertEqual(compare_true("input.exists", True).status, ComparisonStatus.PASS)
        self.assertEqual(compare_true("input.exists", False).status, ComparisonStatus.FAIL)
        with self.assertRaises(TypeError):
            compare_true("input.exists", 1)

    def test_integer_limits_are_exact_and_report_signed_margin(self):
        under = compare_at_most("memory", "16", "12")
        over = compare_at_most("memory", "16", "17")
        enough = compare_at_least("disk", "100", "120")
        short = compare_at_least("disk", "100", "99")
        self.assertEqual(under.delta, -4)
        self.assertEqual(over.status, ComparisonStatus.FAIL)
        self.assertEqual(enough.delta, 20)
        self.assertEqual(short.status, ComparisonStatus.FAIL)


class RationalComparisonTests(unittest.TestCase):
    def test_compares_ratio_by_cross_multiplication(self):
        result = compare_ratio(
            "gpu.coverage",
            expected_numerator=999_993_677_862_271_3,
            expected_denominator=10_000_000_000_000_000,
            actual_numerator="9999936778622713",
            actual_denominator="10000000000000000",
        )
        self.assertEqual(result.status, ComparisonStatus.PASS)

    def test_ratio_denominator_must_be_positive(self):
        with self.assertRaises(ValueError):
            compare_ratio("invalid", 1, 0, 1, 1)


class ExclusionTests(unittest.TestCase):
    def test_only_fixed_nondeterministic_paths_are_excluded(self):
        allowed = {
            "run.started_at_utc",
            "run.duration_ns",
            "process.pid",
            "paths.temporary_directory",
            "metrics.wall_clock_ns",
        }
        for path in allowed:
            with self.subTest(path=path):
                self.assertTrue(is_excluded_path(path))

        rejected = {
            "gpu.resources",
            "gpu.report_hash",
            "source.sha256",
            "domain.quality.reason",
            "paths.session_relative_path",
        }
        for path in rejected:
            with self.subTest(path=path):
                self.assertFalse(is_excluded_path(path))


class OverallStatusTests(unittest.TestCase):
    def test_passed_ignores_explicit_deferred_scope(self):
        overall = aggregate_overall_status(
            [AcceptanceStatus.PASS, AcceptanceStatus.DEFERRED_OUT_OF_SCOPE]
        )
        self.assertEqual(overall, OverallStatus.PASSED)

    def test_correctness_integrity_or_performance_failure_fails_run(self):
        for failure in (
            AcceptanceStatus.FAIL_CORRECTNESS,
            AcceptanceStatus.FAIL_INTEGRITY,
            AcceptanceStatus.FAIL_PERFORMANCE,
        ):
            with self.subTest(failure=failure):
                self.assertEqual(
                    aggregate_overall_status([AcceptanceStatus.PASS, failure]),
                    OverallStatus.FAILED,
                )

    def test_environment_block_or_harness_error_is_incomplete(self):
        for blocked in (
            AcceptanceStatus.BLOCKED_ENVIRONMENT,
            AcceptanceStatus.HARNESS_ERROR,
        ):
            with self.subTest(blocked=blocked):
                self.assertEqual(
                    aggregate_overall_status([AcceptanceStatus.PASS, blocked]),
                    OverallStatus.INCOMPLETE,
                )


if __name__ == "__main__":
    unittest.main()
