import json
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from tools.n30_acceptance.run_acceptance import (
    build_final_result,
    create_run_directory,
    load_comparison_records,
)
from tools.n30_acceptance.status import AcceptanceStatus


class RunDirectoryTests(unittest.TestCase):
    def test_new_run_has_stable_timestamp_and_fingerprint_suffix(self):
        with tempfile.TemporaryDirectory() as directory:
            run = create_run_directory(
                Path(directory),
                "a" * 64,
                now=datetime(2026, 9, 7, 1, 2, 3, tzinfo=timezone.utc),
            )
            self.assertEqual(run.name, "n30a-20260907-010203-aaaaaaaa")
            self.assertTrue(run.is_dir())
            with self.assertRaises(FileExistsError):
                create_run_directory(
                    Path(directory),
                    "a" * 64,
                    now=datetime(2026, 9, 7, 1, 2, 3, tzinfo=timezone.utc),
                )


class FinalResultTests(unittest.TestCase):
    def test_partial_pass_does_not_claim_full_acceptance(self):
        result = build_final_result(
            identity={}, outcomes=[{"phase": "P8", "status": "PASS"}],
            comparisons=[], profile={"phases": ["P0", "P8", "P9"]},
            rerun_command="rerun",
        )
        self.assertEqual(result["overall_status"], "INCOMPLETE")
        self.assertEqual(result["missing_required_phases"], ["P0", "P9"])

    def test_failed_comparison_cannot_hide_behind_passed_phase(self):
        result = build_final_result(
            identity={}, outcomes=[{"phase": "P0", "status": "PASS"}],
            comparisons=[{"name": "bad", "status": "FAIL"}],
            profile={"phases": ["P0"]}, rerun_command="rerun",
        )
        self.assertEqual(result["overall_status"], "INCOMPLETE")
        self.assertIn("unattributed_failed_comparisons", result)

    def test_comparison_records_are_loaded_in_sequence_without_loss(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "comparisons.ndjson"
            path.write_text(
                '{"sequence":1,"name":"a","status":"PASS"}\n'
                '{"sequence":2,"name":"b","status":"FAIL"}\n',
                encoding="utf-8",
            )
            self.assertEqual(
                [item["name"] for item in load_comparison_records(path)], ["a", "b"]
            )

    def test_final_result_aggregates_status_and_keeps_all_comparisons(self):
        result = build_final_result(
            identity={"git_head": "abc"},
            outcomes=[
                {
                    "phase": "P0",
                    "status": AcceptanceStatus.PASS.value,
                    "summary": "ok",
                    "repair_direction": "",
                },
                {
                    "phase": "P4",
                    "status": AcceptanceStatus.FAIL_CORRECTNESS.value,
                    "summary": "mismatch",
                    "repair_direction": "fix converter",
                },
            ],
            comparisons=[{"sequence": 1, "name": "count", "status": "FAIL"}],
            profile={"deferred": ["local-tracy-export"]},
            rerun_command="python run_acceptance.py --resume",
        )
        self.assertEqual(result["overall_status"], "FAILED")
        self.assertEqual(len(result["comparisons"]), 1)
        self.assertEqual(result["deferred"], ["local-tracy-export"])


if __name__ == "__main__":
    unittest.main()
