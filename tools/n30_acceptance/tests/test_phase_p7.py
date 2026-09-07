import io
from pathlib import Path
import tempfile
import unittest

from tools.n30_acceptance.comparison_log import ComparisonRecorder
from tools.n30_acceptance.phase_p7 import (
    RecoveryObservation,
    evaluate_recovery,
    find_single_building,
    parse_cpp_test_summary,
)
from tools.n30_acceptance.status import AcceptanceStatus


class CppRecoverySummaryTests(unittest.TestCase):
    def test_find_single_building_rejects_ambiguity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "case.jn-trace-session"
            (root / "case.jn-trace-session.building.one").mkdir()
            self.assertEqual(
                find_single_building(output).name,
                "case.jn-trace-session.building.one",
            )
            (root / "case.jn-trace-session.building.two").mkdir()
            with self.assertRaisesRegex(ValueError, "exactly one"):
                find_single_building(output)

    def test_parses_machine_readable_check_totals(self):
        self.assertEqual(
            parse_cpp_test_summary(
                "noise\nN30A_CHECK_SUMMARY checks=403 passed=403 failed=0\n"
            ),
            (403, 403, 0),
        )

    def test_missing_summary_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "summary"):
            parse_cpp_test_summary("Trace Session Inventory tests passed\n")


class RecoveryEvaluationTests(unittest.TestCase):
    def _passing(self):
        return RecoveryObservation(
            cpp_exit_code=0,
            cpp_checks=403,
            cpp_passed=403,
            cpp_failed=0,
            single_cancel_exit_code=130,
            single_break_count=1,
            single_state=9,
            current_absent_after_single=True,
            double_cancel_exit_code=130,
            double_break_count=2,
            double_state=9,
            current_absent_after_double=True,
            resume_exit_code=0,
            published=True,
            publishable_state=True,
            source_sha_unchanged=True,
            response_hashes_equal=True,
        )

    def test_each_recovery_fact_is_printed_and_passes(self):
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=console)
            outcome = evaluate_recovery(self._passing(), recorder=recorder)
            self.assertEqual(outcome.status, AcceptanceStatus.PASS)
            rendered = console.getvalue()
            self.assertIn("P7.single_cancel.exit_code", rendered)
            self.assertIn("P7.double_cancel.break_count", rendered)
            self.assertIn("P7.resume.response_hashes_equal", rendered)

    def test_unsafe_publish_after_cancel_is_integrity_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            value = self._passing()
            value = RecoveryObservation(**{**value.__dict__, "current_absent_after_single": False})
            outcome = evaluate_recovery(value, recorder=recorder)
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_INTEGRITY)

    def test_empty_cpp_suite_cannot_pass_vacuously(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            value = self._passing()
            value = RecoveryObservation(
                **{**value.__dict__, "cpp_checks": 0, "cpp_passed": 0}
            )
            outcome = evaluate_recovery(value, recorder=recorder)
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_CORRECTNESS)


if __name__ == "__main__":
    unittest.main()
