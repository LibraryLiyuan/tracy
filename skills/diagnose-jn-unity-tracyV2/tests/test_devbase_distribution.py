"""Dev-based distribution contract; does not read or scan a Trace."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class DevBaseDistributionTests(unittest.TestCase):
    def test_runtime_instructions_do_not_advertise_session_input(self):
        for relative in ("SKILL.md", "references/query-native-scan-workflow.md"):
            with self.subTest(path=relative):
                text = (ROOT / relative).read_text(encoding="utf-8")
                self.assertNotIn("已发布 N30 Session", text)
                self.assertNotIn("或已发布 Session", text)
                self.assertNotIn("Trace / completed Session", text)
                self.assertIn("仅支持普通 `.tracy`", text)

    def test_local_runtime_profile_is_not_distributed(self):
        ignored = (ROOT / "config/.gitignore").read_text(encoding="utf-8")
        self.assertIn("JNTracy.AnalysisProfile.yaml", ignored.splitlines())
        self.assertTrue((ROOT / "config/JNTracy.AnalysisProfile.example.yaml").is_file())


if __name__ == "__main__":
    unittest.main()
