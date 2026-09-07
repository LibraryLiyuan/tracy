import unittest


from tools.n30_acceptance.orchestrator import (
    PHASE_ORDER,
    dependency_allows,
    evaluate_disk_preflight,
    select_phases,
)
from tools.n30_acceptance.status import AcceptanceStatus


class PhaseSelectionTests(unittest.TestCase):
    def test_from_phase_selects_tail(self):
        self.assertEqual(
            select_phases(PHASE_ORDER, from_phase="P4"),
            ("P4", "P5", "P6", "P7", "P8", "P9"),
        )

    def test_rerun_phase_selects_phase_and_report_only(self):
        self.assertEqual(
            select_phases(PHASE_ORDER, rerun_phase="P5"), ("P5", "P9")
        )

    def test_unknown_or_conflicting_selection_is_rejected(self):
        with self.assertRaises(ValueError):
            select_phases(PHASE_ORDER, from_phase="P99")
        with self.assertRaises(ValueError):
            select_phases(PHASE_ORDER, from_phase="P2", rerun_phase="P3")


class DependencyTests(unittest.TestCase):
    def test_identity_or_harness_failure_stops_every_dependent_phase(self):
        self.assertFalse(
            dependency_allows("P0", AcceptanceStatus.FAIL_INTEGRITY, "P1")
        )
        self.assertFalse(
            dependency_allows("P1", AcceptanceStatus.HARNESS_ERROR, "P5")
        )

    def test_short_oracle_failure_allows_read_only_a7_audit_but_not_heavy_work(self):
        self.assertFalse(
            dependency_allows("P3", AcceptanceStatus.FAIL_CORRECTNESS, "P4")
        )
        self.assertTrue(
            dependency_allows("P3", AcceptanceStatus.FAIL_CORRECTNESS, "P5")
        )
        self.assertFalse(
            dependency_allows("P3", AcceptanceStatus.FAIL_CORRECTNESS, "P8")
        )


class DiskPreflightTests(unittest.TestCase):
    def test_requires_scratch_budget_and_larger_of_fixed_or_percent_reserve(self):
        gib = 1024**3
        result = evaluate_disk_preflight(
            volume_capacity_bytes=4_000 * gib,
            volume_free_bytes=1_000 * gib,
            scratch_hard_bytes=320 * gib,
            minimum_reserve_bytes=64 * gib,
            minimum_reserve_percent=10,
        )
        self.assertEqual(result.required_reserve_bytes, 400 * gib)
        self.assertEqual(result.required_free_bytes, 720 * gib)
        self.assertTrue(result.passed)

        failed = evaluate_disk_preflight(
            volume_capacity_bytes=4_000 * gib,
            volume_free_bytes=700 * gib,
            scratch_hard_bytes=320 * gib,
            minimum_reserve_bytes=64 * gib,
            minimum_reserve_percent=10,
        )
        self.assertFalse(failed.passed)


if __name__ == "__main__":
    unittest.main()
