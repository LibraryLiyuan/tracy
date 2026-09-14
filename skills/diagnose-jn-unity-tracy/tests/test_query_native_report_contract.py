import copy
import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tests.test_report_builder import valid_inputs, write_json


SKILL_ROOT = Path(__file__).resolve().parents[1]
BUILD_REPORT = SKILL_ROOT / "scripts" / "build_report.py"
CONTRACT_PATH = SKILL_ROOT / "scripts" / "query_native_report.py"


def load_contract():
    if str(CONTRACT_PATH.parent) not in sys.path:
        sys.path.insert(0, str(CONTRACT_PATH.parent))
    spec = importlib.util.spec_from_file_location("jntracy_query_native_report", CONTRACT_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load query-native report contract")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def candidate_manifest() -> dict:
    trigger = {
        "trigger": "budget",
        "metric": "gpu_exclusive_ns",
        "scope": "complete_frames",
        "reason": "frame budget exceeded",
        "observed_value": "13000000",
        "threshold_value": "10000000",
        "unit": "ns",
        "authority": "candidate-policy-v3",
        "eligible": True,
    }
    return {
        "schema_version": 1,
        "aggregate_identity": "1" * 64,
        "aggregate_content_sha256": "2" * 64,
        "policy_identity": "3" * 64,
        "profile_identity": "4" * 64,
        "policy_algorithm": "candidate-policy-v3",
        "content_sha256": "5" * 64,
        "ranked_signatures": [],
        "candidates": [
            {
                "candidate_id": "candidate-tsr",
                "family_id": "family-tsr",
                "priority": "P1",
                "domain": "gpu",
                "signature_id": "SIG-0001",
                "structural_signature": "GPU.Direct/PostProcessing/TSR",
                "member_signatures": ["SIG-0001"],
                "triggers": ["budget"],
                "trigger_evidence": [trigger],
                "representative_frames": ["42", "1042", "642"],
                "representative_frame_details": [
                    {"frame": "42", "reasons": ["typical_bad"], "event_refs": ["zone-tsr"]},
                    {"frame": "1042", "reasons": ["worst"], "event_refs": ["zone-tsr"]},
                    {"frame": "642", "reasons": ["last"], "event_refs": ["zone-tsr"]},
                ],
                "selected": True,
                "not_selected_reason": None,
                "quality_status": "complete",
            }
        ],
        "backlog": {"total": 1, "selected": 1, "not_selected": 0},
    }


def _legacy_query_native_analysis(root: Path) -> tuple[Path, Path, Path, dict, dict]:
    analysis_path, evidence_path = valid_inputs(root)
    analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
    manifest = candidate_manifest()
    manifest_path = root / "candidate-manifest.json"
    write_json(manifest_path, manifest)
    analysis["report_identity"]["configuration"] = {
        "analysis_profile": {
            "path": "config/JNTracy.AnalysisProfile.yaml",
            "sha256": "6" * 64,
        }
    }
    analysis["query_scan"] = {
        "scan_id": "scan-1",
        "aggregate_content_sha256": manifest["aggregate_content_sha256"],
        "candidate_content_sha256": manifest["content_sha256"],
        "candidate_manifest_path": "candidate-manifest.json",
        "candidate_manifest_sha256": hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
        "coverage": {
            "candidate_total": 1,
            "required_total": 1,
            "required_completed": 1,
            "investigated_total": 1,
            "uninvestigated_total": 0,
        },
    }
    analysis["candidate_findings"] = [
        {
            "candidate_id": "candidate-tsr",
            "signature_id": "SIG-0001",
            "priority": "P1",
            "conclusion_status": "Supported",
            "query_numeric_facts": copy.deepcopy(manifest["candidates"][0]["trigger_evidence"]),
            "representative_frames": copy.deepcopy(manifest["candidates"][0]["representative_frame_details"]),
            "representative_selection_reason": "Query chose typical, worst, and a later repeated instance.",
            "analysis_trail": {
                "discovery": "Budget trigger selected this GPU family.",
                "statistics": "Query reports the candidate-owned exact values above.",
                "representative_frames": "Three structurally matching frames were inspected.",
                "limiter_analysis": "The typed wait chain reaches the TSR GPU zone.",
                "hypotheses": "TSR critical-path hypothesis was tested.",
                "source_analysis": "No source conclusion was required.",
                "counterevidence": "No hardware counter evidence for bandwidth limitation.",
                "conclusion": "TSR is supported as the limiter for this candidate family.",
            },
            "confirmation_basis": ["typed_relation"],
            "evidence_refs": ["EVD-TIMELINE-001"],
            "source_evidence": [],
        }
    ]
    analysis["investigation_backlog"] = []
    analysis["report_versions"]["analysis_workflow"] = "2.0.0"
    write_json(analysis_path, analysis)
    return analysis_path, evidence_path, manifest_path, analysis, manifest


def query_native_analysis(root):
    from tests.review_fixtures import current_policy_fixture
    return current_policy_fixture(root)


class QueryNativeReportContractTests(unittest.TestCase):
    def test_validation_machine_output_distinguishes_stage_from_complete(self):
        # A passing format/receipt check is not completion of all investigations.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ap, ep, cp, analysis, _ = query_native_analysis(root)
            analysis["report_status"] = "in_progress"
            analysis["candidate_findings"] = []
            analysis["bottleneck_signatures"] = []
            analysis["evidence_cards"] = []
            analysis["investigation_backlog"] = [{"candidate_id": "candidate-tsr", "reason": "not_investigated"}]
            analysis["query_scan"]["coverage"].update(required_completed=0, investigated_total=0, uninvestigated_total=1)
            write_json(ap, analysis)
            for script, extra in [(BUILD_REPORT, ["--validate-only"]),
                    (SKILL_ROOT / "scripts" / "validate_analysis_result.py", [])]:
                result = subprocess.run([sys.executable, "-B", str(script), "--legacy-regression", "--analysis", str(ap),
                    "--evidence", str(ep), "--candidates", str(cp), *extra], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                status = json.loads(result.stdout)
                self.assertTrue(status["passed"])
                self.assertEqual(status.get("report_status"), "in_progress")
                self.assertIs(status.get("analysis_complete"), False)
                self.assertEqual(status.get("required_pending"), 1)
            output = root / "stage-report"
            rendered = subprocess.run([sys.executable, "-B", str(BUILD_REPORT), "--legacy-regression", "--analysis", str(ap),
                "--evidence", str(ep), "--candidates", str(cp), "--output", str(output), "--deterministic"],
                capture_output=True, text=True)
            self.assertEqual(rendered.returncode, 0, rendered.stderr)
            status = json.loads((output / "report-validation.json").read_text(encoding="utf-8"))
            self.assertTrue(status["passed"])
            self.assertEqual(status["report_status"], "in_progress")
            self.assertFalse(status["analysis_complete"])
            self.assertEqual(status["required_pending"], 1)

    def test_complete_validation_and_failed_validation_report_completion_honestly(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ap, ep, cp, analysis, _ = query_native_analysis(root)
            command = [sys.executable, "-B", str(SKILL_ROOT / "scripts" / "validate_analysis_result.py"), "--legacy-regression",
                "--analysis", str(ap), "--evidence", str(ep), "--candidates", str(cp), "--output", str(root / "validation.json")]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            status = json.loads((root / "validation.json").read_text(encoding="utf-8"))
            self.assertFalse(status["analysis_complete"])
            self.assertEqual(status["required_pending"], 0)
            # Claimed complete coverage cannot override an actual numeric audit error.
            analysis["candidate_findings"][0]["query_numeric_facts"][0]["observed_value"] = "999"
            write_json(ap, analysis)
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            status = json.loads((root / "validation.json").read_text(encoding="utf-8"))
            self.assertFalse(status["passed"])
            self.assertFalse(status["analysis_complete"])

    def validate(self, analysis: dict, manifest: dict, root: Path) -> list[str]:
        return load_contract().validate_query_native_analysis(analysis, manifest, json.loads((root / "evidence-manifest.json").read_text()), root)

    def test_stage_report_retains_pending_without_claiming_completion(self):
        with tempfile.TemporaryDirectory() as directory:
            _, _, _, analysis, manifest = query_native_analysis(Path(directory))
            analysis["report_status"] = "in_progress"
            analysis["candidate_findings"] = []
            analysis["bottleneck_signatures"] = []
            analysis["investigation_backlog"] = [{"candidate_id": "candidate-tsr", "reason": "not_investigated"}]
            analysis["query_scan"]["coverage"].update(required_completed=0, investigated_total=0, uninvestigated_total=1)
            self.assertEqual([], self.validate(analysis, manifest, Path(directory)))
            analysis["report_status"] = "complete"
            self.assertTrue(self.validate(analysis, manifest, Path(directory)))

    def test_v3_prose_only_finding_does_not_count_as_investigated(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, _, _, analysis, manifest = query_native_analysis(Path(directory))
            manifest["policy_algorithm"] = "candidate-policy-v3"
            analysis["candidate_findings"][0].pop("investigation_receipts")
            self.assertTrue(any("receipt" in e for e in self.validate(analysis, manifest, Path(directory))))

    def test_selected_p2_in_backlog_cannot_complete_report(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, _, _, analysis, manifest = query_native_analysis(Path(directory))
            candidate = manifest["candidates"][0]
            candidate["priority"] = "P2"
            analysis["candidate_findings"] = []
            analysis["bottleneck_signatures"] = []
            analysis["investigation_backlog"] = [{"candidate_id": "candidate-tsr", "reason": "time_limit"}]
            analysis["query_scan"]["coverage"] = {
                "candidate_total": 1, "required_total": 0, "required_completed": 0,
                "investigated_total": 0, "uninvestigated_total": 1,
            }
            self.assertTrue(any("no completed investigation" in e for e in self.validate(analysis, manifest, Path(directory))),
                            "Selected P2 must not be satisfied by a backlog reason")

    def test_missing_candidate_trigger_path_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, _, _, analysis, manifest = query_native_analysis(Path(directory))
            analysis["candidate_findings"][0]["query_numeric_facts"] = []
            self.assertTrue(any("query_numeric_facts" in e for e in self.validate(analysis, manifest, Path(directory))))

    def test_missing_representative_selection_reason_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, _, _, analysis, manifest = query_native_analysis(Path(directory))
            analysis["candidate_findings"][0]["representative_selection_reason"] = ""
            self.assertTrue(any("representative_selection_reason" in e for e in self.validate(analysis, manifest, Path(directory))))

    def test_ai_numeric_rewrite_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, _, _, analysis, manifest = query_native_analysis(Path(directory))
            analysis["candidate_findings"][0]["query_numeric_facts"][0]["observed_value"] = "13000001"
            self.assertTrue(any("query_numeric_facts" in e for e in self.validate(analysis, manifest, Path(directory))))

    def test_confirmed_requires_auditable_confirmation_basis(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, _, _, analysis, manifest = query_native_analysis(Path(directory))
            finding = analysis["candidate_findings"][0]
            finding["conclusion_status"] = "Confirmed"
            finding["confirmation_basis"] = []
            self.assertTrue(any("confirmation_basis" in e for e in self.validate(analysis, manifest, Path(directory))))

    def test_source_revision_mismatch_cannot_confirm_source_claim(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, _, _, analysis, manifest = query_native_analysis(Path(directory))
            finding = analysis["candidate_findings"][0]
            finding["conclusion_status"] = "Confirmed"
            finding["confirmation_basis"] = ["source_revision_match"]
            finding["source_evidence"] = [
                {"source_id": "SRC-1", "revision_match": False, "used_for_confirmation": True}
            ]
            self.assertTrue(any("revision" in e for e in self.validate(analysis, manifest, Path(directory))))

    def test_capture_quality_cannot_be_a_performance_finding(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            analysis_path, evidence_path, manifest_path, analysis, manifest = query_native_analysis(root)
            candidate = manifest["candidates"][0]
            candidate["priority"] = "P0"
            candidate["domain"] = "quality"
            candidate["representative_frames"] = []
            candidate["representative_frame_details"] = []
            candidate["triggers"] = ["quality"]
            finding = analysis["candidate_findings"][0]
            finding["priority"] = "P0"
            finding["representative_frames"] = []
            finding["representative_selection_reason"] = (
                "This is a capture-level exact audit finding, so no individual frame is authoritative."
            )
            write_json(manifest_path, manifest)
            analysis["query_scan"]["candidate_manifest_sha256"] = hashlib.sha256(
                manifest_path.read_bytes()
            ).hexdigest()
            write_json(analysis_path, analysis)
            result = subprocess.run(
                [
                    sys.executable,
                    str(BUILD_REPORT), "--legacy-regression",
                    "--analysis", str(analysis_path),
                    "--evidence", str(evidence_path),
                    "--candidates", str(manifest_path),
                    "--validate-only",
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0, "Quality findings must be rejected, not investigated as P0")

    def test_capture_quality_is_preserved_separately_without_required_investigation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, _, _, analysis, manifest = query_native_analysis(root)
            quality = [{"domain": "gpu.catalog", "status": "invalid", "reason": "catalog core gap"}]
            manifest["capture_quality"] = quality
            self.assertTrue(any("capture_quality" in error for error in self.validate(analysis, manifest, Path(directory))))
            analysis["capture_quality"] = quality
            self.assertEqual(self.validate(analysis, manifest, Path(directory)), [])
            self.assertEqual(analysis["candidate_findings"][0]["priority"], "P1")

    def test_quality_appendix_is_rendered_in_main_and_quality_attachment(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            analysis_path, evidence_path, manifest_path, analysis, manifest = query_native_analysis(root)
            quality = [{"domain": "gpu.catalog", "status": "invalid", "reason": "catalog core gap"}]
            manifest["capture_quality"] = quality
            analysis["capture_quality"] = quality
            write_json(manifest_path, manifest)
            analysis["query_scan"]["candidate_manifest_sha256"] = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
            write_json(analysis_path, analysis)
            output = root / "report"
            result = subprocess.run([sys.executable, str(BUILD_REPORT), "--legacy-regression", "--analysis", str(analysis_path),
                "--evidence", str(evidence_path), "--candidates", str(manifest_path),
                "--output", str(output), "--deterministic"], text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            main = (output / "Performance-Analysis-Report.md").read_text(encoding="utf-8")
            self.assertIn("catalog core gap", main.split("## 11.", 1)[1])
            self.assertNotIn("catalog core gap", main.split("## 11.", 1)[0])
            self.assertIn("catalog core gap", (output / "01-Trace-Quality-and-Scan-Coverage.md").read_text(encoding="utf-8"))
            self.assertNotIn("catalog core gap", (output / "Analysis-Process-and-Query-Audit.md").read_text(encoding="utf-8"))

    def test_job_candidate_without_bottleneck_signature_is_rendered_in_job_report(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            analysis_path, evidence_path, manifest_path, analysis, manifest = query_native_analysis(root)
            from tests.job_report_fixture import attach_job
            job_signature = attach_job(root, analysis, manifest, evidence_path)
            analysis["bottleneck_signatures"] = []
            analysis["evidence_cards"] = []
            write_json(manifest_path, manifest)
            analysis["query_scan"]["candidate_manifest_sha256"] = hashlib.sha256(
                manifest_path.read_bytes()
            ).hexdigest()
            write_json(analysis_path, analysis)

            output = root / "report"
            result = subprocess.run(
                [
                    sys.executable,
                    str(BUILD_REPORT), "--legacy-regression",
                    "--analysis", str(analysis_path),
                    "--evidence", str(evidence_path),
                    "--candidates", str(manifest_path),
                    "--output", str(output),
                    "--deterministic",
                    "--export-static",
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            job_report = (output / "04-Job-and-Scheduling-Analysis.md").read_text(encoding="utf-8")
            self.assertIn(job_signature, job_report)
            self.assertNotIn("本专项没有已完成的 Candidate 调查", job_report)

    def test_query_native_build_emits_main_and_nine_deterministic_reports(self) -> None:
        contract = load_contract()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            analysis, evidence, candidates, _, _ = query_native_analysis(root)
            outputs = [root / "report-a", root / "report-b"]
            for output in outputs:
                result = subprocess.run(
                    [
                        sys.executable,
                        str(BUILD_REPORT), "--legacy-regression",
                        "--analysis", str(analysis),
                        "--evidence", str(evidence),
                        "--candidates", str(candidates),
                        "--output", str(output),
                        "--deterministic",
                        "--export-static",
                    ],
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue((output / "Performance-Analysis-Report.md").is_file())
                for name in contract.SPECIALTY_REPORT_FILES:
                    self.assertTrue((output / name).is_file(), name)
            names = ["Performance-Analysis-Report.md", *contract.SPECIALTY_REPORT_FILES]
            self.assertEqual(
                {name: hashlib.sha256((outputs[0] / name).read_bytes()).hexdigest() for name in names},
                {name: hashlib.sha256((outputs[1] / name).read_bytes()).hexdigest() for name in names},
            )
            main = (outputs[0] / "Performance-Analysis-Report.md").read_text(encoding="utf-8")
            self.assertIn("分析过程", main)
            self.assertIn("Budget trigger selected", main)
            manual = (outputs[0] / "08-Profiler-Manual-Verification-Guide.md").read_text(encoding="utf-8")
            self.assertIn("Frame 42", manual)
            self.assertIn("GPU Direct", manual)


if __name__ == "__main__":
    unittest.main()
