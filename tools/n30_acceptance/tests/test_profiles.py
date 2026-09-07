import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ProfileTests(unittest.TestCase):
    def test_full_profile_defines_all_phases_and_hard_limits(self):
        profile = json.loads(
            (ROOT / "profiles" / "n30a-full.json").read_text(encoding="utf-8")
        )
        self.assertEqual(profile["profile_schema"], 1)
        self.assertEqual(
            profile["phases"],
            ["P0", "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8", "P9"],
        )
        self.assertEqual(profile["query"]["attempts"], 3)
        self.assertEqual(profile["query"]["max_response_bytes"], 16 * 1024 * 1024)
        self.assertEqual(profile["converter"]["hard_private_bytes"], 16 * 1024**3)
        self.assertEqual(profile["converter"]["outer_job_bytes"], 18 * 1024**3)
        self.assertEqual(profile["disk"]["scratch_hard_bytes"], 320 * 1024**3)

    def test_a7_golden_uses_exact_decimal_strings(self):
        golden = json.loads(
            (ROOT / "profiles" / "expected-n30a-a7.json").read_text(encoding="utf-8")
        )
        self.assertEqual(golden["source"]["size"], "19122822908")
        self.assertEqual(
            golden["source"]["sha256"],
            "2ac46c53257ce9027ec67e098fc15070fb911243f6cd311a166eb97547848068",
        )
        self.assertEqual(golden["metrics"]["trace.counts.cpu_zones"], "234691471")
        self.assertEqual(golden["metrics"]["gpu.verifier.report_hash"], "9444475405900354442")
        self.assertEqual(golden["metrics"]["session.indexed_records"], "2285222848")
        for key, value in golden["metrics"].items():
            with self.subTest(key=key):
                self.assertIsInstance(value, str)
                self.assertRegex(value, r"^-?[0-9]+$")


if __name__ == "__main__":
    unittest.main()
