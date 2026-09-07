import json
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET


from tools.n30_acceptance.report_writer import write_final_reports


class ReportWriterTests(unittest.TestCase):
    def test_writes_machine_json_junit_markdown_and_hash_manifest(self):
        result = {
            "run_id": "run-fixture",
            "overall_status": "FAILED",
            "scope": ["N30A"],
            "deferred": ["local-tracy-export", "profiler-session-ui"],
            "identity": {"git_head": "abc123", "source_sha256": "f" * 64},
            "phases": [
                {"phase": "P0", "status": "PASS", "summary": "identity locked"},
                {
                    "phase": "P5",
                    "status": "FAIL_CORRECTNESS",
                    "summary": "gpu count mismatch",
                    "repair_direction": "inspect canonical GPU batch accounting",
                },
            ],
            "comparisons": [
                {
                    "name": "gpu.resources",
                    "status": "FAIL",
                    "rule": "exact_integer",
                    "expected": 7,
                    "actual": 6,
                    "delta": -1,
                    "detail": "",
                }
            ],
            "comparison_exclusions": ["process.pid"],
        }
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "report"
            (report.parent / "raw").mkdir()
            (report.parent / "raw" / "query-response.log").write_text("exact source evidence")
            outputs = write_final_reports(report, result)

            machine = json.loads(outputs.result_json.read_text(encoding="utf-8"))
            self.assertEqual(machine["overall_status"], "FAILED")

            junit = ET.parse(outputs.junit_xml).getroot()
            self.assertEqual(junit.attrib["failures"], "1")
            self.assertEqual(junit.attrib["errors"], "0")

            markdown = outputs.markdown.read_text(encoding="utf-8")
            self.assertIn("gpu.resources", markdown)
            self.assertIn("期望值", markdown)
            self.assertIn("inspect canonical GPU batch accounting", markdown)
            self.assertIn("local-tracy-export", markdown)

            manifest = outputs.evidence_sha256.read_text(encoding="utf-8")
            self.assertIn("acceptance-result.json", manifest)
            self.assertIn("acceptance-junit.xml", manifest)
            self.assertIn("../raw/query-response.log", manifest)
            self.assertIn("全域计数与源端退化", markdown)
            self.assertIn("进程时间与资源", markdown)
            self.assertNotIn("EVIDENCE-SHA256.txt  EVIDENCE-SHA256.txt", manifest)


if __name__ == "__main__":
    unittest.main()
