import importlib.util
import copy
import json
import tempfile
import unittest
from pathlib import Path


SKILL_ROOT = Path(__file__).resolve().parents[1]
SKILL_PATH = SKILL_ROOT / "SKILL.md"
STATE_TEMPLATE = SKILL_ROOT / "assets" / "analysis-state-template.json"
STATE_TOOL = SKILL_ROOT / "scripts" / "manage-analysis-state.py"
PROFILE_PATH = SKILL_ROOT / "config" / "JNTracy.AnalysisProfile.example.yaml"


def load_state_tool():
    spec = importlib.util.spec_from_file_location("jntracy_analysis_state", STATE_TOOL)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load analysis state tool")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def candidate(candidate_id, priority, triggers, *, selected=True):
    return {
        "candidate_id": candidate_id,
        "family_id": f"family:{candidate_id}",
        "priority": priority,
        "domain": "cpu",
        "signature_id": f"sig:{candidate_id}",
        "structural_signature": f"CPU/Main/{candidate_id}",
        "member_signatures": [f"sig:{candidate_id}"],
        "triggers": triggers,
        "trigger_evidence": [
            {
                "trigger": triggers[0],
                "metric": "exclusive_ns",
                "scope": "complete_frames",
                "reason": "fixture",
                "observed_value": "2500000",
                "threshold_value": "1000000",
                "unit": "ns",
                "authority": "candidate-policy-v1",
                "eligible": True,
            }
        ],
        "representative_frames": ["42"],
        "representative_frame_details": [
            {"frame": "42", "reasons": ["worst"], "event_refs": [f"event:{candidate_id}"]}
        ],
        "selected": selected,
        "not_selected_reason": None if selected else "outside_top_policy",
        "quality_status": "complete",
    }


class SkillV2ContractTests(unittest.TestCase):
    def test_v3_completed_ids_without_artifact_audit_are_rejected(self):
        tool = load_state_tool()
        state = tool.new_state(STATE_TEMPLATE)
        state["scan"]["policy_algorithm"] = "candidate-policy-v3"
        state["task"]["state"] = "completed"
        state["investigations"]["coverage"] = {"required_total": 1, "required_completed": 1}
        state["investigations"]["completed"] = [{"candidate_id": "forged-only-id"}]
        self.assertTrue(any("audit" in e for e in tool.validate_state(state)))

    def test_selected_p2_is_required_and_terminal_state_rejects_pending(self) -> None:
        tool = load_state_tool()
        state = tool.new_state(STATE_TEMPLATE)
        tool.ingest_candidate_pages(state, [{"items": [candidate("local-p2", "P2", ["top"])],
            "page": {"done": True, "next_cursor": None, "total": "1"}}])
        self.assertTrue(state["investigations"]["queue"][0]["required"])
        self.assertEqual(state["investigations"]["coverage"]["required_total"], 1)
        self.assertEqual(tool.validate_state(state), [], "In-progress work remains resumable")
        state["task"]["state"] = "completed"
        self.assertTrue(any("pending" in e for e in tool.validate_state(state)))

    def test_description_is_only_a_single_trace_routing_signal(self) -> None:
        text = SKILL_PATH.read_text(encoding="utf-8")
        frontmatter = text.split("---", 2)[1]
        description = next(
            line.split(":", 1)[1].strip()
            for line in frontmatter.splitlines()
            if line.startswith("description:")
        )
        self.assertTrue(description.startswith("Use when "))
        self.assertRegex(description.lower(), r"(single|单个|单次).*(trace|capture|捕获)")
        self.assertNotIn("A/B", description)
        self.assertNotIn("不用于", description)
        self.assertIn("version: 2.0.1", frontmatter)

    def test_schema4_and_yaml_are_the_only_runtime_authorities(self) -> None:
        text = SKILL_PATH.read_text(encoding="utf-8")
        template = json.loads(STATE_TEMPLATE.read_text(encoding="utf-8"))
        self.assertEqual(template["schema_version"], 4)
        self.assertIn("JNTracy.AnalysisProfile.yaml", text)
        for legacy in (
            "config/local-profile.json",
            "config/project-profile.json",
            "config/performance-budgets.json",
            "config/marker-attribution.json",
            "resolve-profile.ps1",
        ):
            self.assertNotIn(legacy, text)

    def test_missing_profile_and_wrong_query_schema_stop_preflight(self) -> None:
        tool = load_state_tool()
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "missing.yaml"
            with self.assertRaises(tool.AnalysisStateError):
                tool.require_profile(missing)
        self.assertTrue(PROFILE_PATH.is_file())
        with self.assertRaises(tool.AnalysisStateError):
            tool.require_query_schema({"query_schema": "1.34.0"})
        self.assertEqual(tool.require_query_schema({"query_schema": "1.35.0"}), "1.35.0")

    def test_scan_resume_and_aggregate_reuse_are_idempotent(self) -> None:
        tool = load_state_tool()
        state = tool.new_state(STATE_TEMPLATE)
        tool.apply_scan_status(
            state,
            {"scan_id": "scan-1", "state": "CancelledResumable", "resumable": True,
             "completed": False, "progress": {"completed": "2", "total": "4", "stage": "cancelled"}},
        )
        self.assertEqual(tool.next_scan_action(state), "resume")

        summary = {
            "scan_id": "scan-1",
            "aggregate_content_sha256": "a" * 64,
            "candidate_content_sha256": "b" * 64,
            "quality": {"complete": True},
            "domains": [],
            "backlog": {"total": 1, "selected": 1, "not_selected": 0},
        }
        tool.apply_scan_summary(state, summary)
        history_count = len(state["task"]["state_history"])
        tool.apply_scan_summary(state, summary)
        self.assertEqual(len(state["task"]["state_history"]), history_count)
        self.assertEqual(state["scan"]["aggregate_content_sha256"], "a" * 64)

    def test_all_p0_p1_and_user_focus_candidates_enter_queue_once(self) -> None:
        tool = load_state_tool()
        candidates = [
            candidate("p0", "P0", ["user_focus"]),
            candidate("p1", "P1", ["budget"]),
            candidate("focus", "P3", ["user_focus"], selected=False),
            candidate("selected-p2", "P2", ["top"]),
            candidate("backlog-p4", "P4", ["top"], selected=False),
        ]
        state = tool.new_state(STATE_TEMPLATE)
        state["investigations"]["completed"] = [
            {"candidate_id": "p0", "status": "Confirmed", "completed_at": "2026-09-06T00:00:00Z"}
        ]
        tool.ingest_candidate_pages(
            state,
            [{"items": candidates, "page": {"cursor": "", "next_cursor": None, "done": True, "total": "5"}}],
        )
        queue_ids = [item["candidate_id"] for item in state["investigations"]["queue"]]
        backlog_ids = [item["candidate_id"] for item in state["investigations"]["backlog"]]
        self.assertNotIn("p0", queue_ids)
        self.assertEqual(queue_ids, ["p1", "focus", "selected-p2"])
        self.assertEqual(backlog_ids, ["p1", "focus", "selected-p2", "backlog-p4"])
        self.assertTrue(all(cid in {"p0", *queue_ids} for cid in {"p0", "p1", "focus"}))

        tool.ingest_candidate_pages(
            state,
            [{"items": candidates, "page": {"cursor": "", "next_cursor": None, "done": True, "total": "5"}}],
        )
        self.assertEqual(
            [item["candidate_id"] for item in state["investigations"]["queue"]], queue_ids
        )

    def test_ai_cannot_rewrite_query_numeric_facts(self) -> None:
        tool = load_state_tool()
        manifest = {"candidates": [candidate("candidate-1", "P1", ["budget"])]}
        analysis = {
            "candidate_findings": [
                {
                    "candidate_id": "candidate-1",
                    "query_numeric_facts": copy.deepcopy(
                        manifest["candidates"][0]["trigger_evidence"]
                    ),
                }
            ]
        }
        self.assertEqual(tool.validate_candidate_numeric_facts(analysis, manifest), [])
        analysis["candidate_findings"][0]["query_numeric_facts"][0]["observed_value"] = "2500001"
        errors = tool.validate_candidate_numeric_facts(analysis, manifest)
        self.assertTrue(any("query_numeric_facts" in error for error in errors))

    def test_legacy_quality_candidates_require_new_policy_not_an_investigation(self) -> None:
        tool = load_state_tool()
        state = tool.new_state(STATE_TEMPLATE)
        items = [candidate("quality", "P0", ["quality"]), candidate("cpu", "P1", ["budget"])]
        with self.assertRaisesRegex(tool.AnalysisStateError, "quality.*candidate-policy-v2"):
            tool.ingest_candidate_pages(state, [{"items": items, "page": {
                "total": "2", "done": True, "next_cursor": None}}])
        self.assertEqual(state["investigations"]["queue"], [])

    def test_quality_summary_does_not_change_priorities_or_coverage(self) -> None:
        tool = load_state_tool()
        state = tool.new_state(STATE_TEMPLATE)
        state["scan"]["scan_id"] = "scan-1"
        quality = [{"domain": "gpu.catalog", "status": "invalid", "reason": "core gap"}]
        tool.apply_scan_summary(state, {"scan_id": "scan-1", "aggregate_content_sha256": "a" * 64,
            "candidate_content_sha256": "b" * 64, "capture_quality": quality})
        items = [candidate("cpu", "P1", ["budget"]), candidate("render", "P2", ["top"])]
        tool.ingest_candidate_pages(state, [{"items": items, "page": {
            "total": "2", "done": True, "next_cursor": None}}])
        self.assertEqual(state["capture_quality"], quality)
        self.assertEqual([item["priority"] for item in state["investigations"]["queue"]], ["P1", "P2"])
        self.assertEqual(state["investigations"]["coverage"]["candidate_total"], 2)
        self.assertEqual(state["investigations"]["coverage"]["required_total"], 2)


if __name__ == "__main__":
    unittest.main()
