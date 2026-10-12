"""Query 1.35 ScanStateName wire values must drive recovery, not poll forever."""
import copy
import importlib.util
from pathlib import Path
import unittest

SKILL_ROOT = Path(__file__).resolve().parents[1]
STATE_TEMPLATE = SKILL_ROOT / 'assets/analysis-state-template.json'


def load_state_tool():
    spec = importlib.util.spec_from_file_location('wire_status_state_tool', SKILL_ROOT / 'scripts/manage-analysis-state.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ScanWireStatusTests(unittest.TestCase):
    def setUp(self):
        self.tool = load_state_tool()
        self.state = self.tool.new_state(STATE_TEMPLATE)

    def response(self, name, completed=False, resumable=False):
        return {"scan_id": "scan-wire", "state": name, "completed": completed,
                "resumable": resumable, "closed": False, "error": None,
                "progress": {"completed": "4", "total": "4", "stage": name}}

    def test_wire_terminal_states_route_without_rewriting_evidence(self):
        for name, done, resume, task, action in [
            ("complete", True, False, "scan_complete", "read_summary_and_candidates"),
            ("cancelled_resumable", False, True, "scan_cancelled_resumable", "resume"),
            ("failed", False, False, "scan_failed", "stop"),
        ]:
            with self.subTest(state=name):
                response = self.response(name, done, resume)
                original = copy.deepcopy(response)
                self.tool.apply_scan_status(self.state, response, "saved-status.json")
                self.assertEqual(self.state["task"]["state"], task)
                self.assertEqual(self.tool.next_scan_action(self.state), action)
                self.assertEqual(self.state["scan"]["state"], name)
                self.assertEqual(response, original)
                self.assertEqual(self.state["task"]["state_history"][-1]["evidence"], "saved-status.json")

    def test_legacy_saved_terminal_states_remain_resumable(self):
        for name, done, resume, action in [
            ("Complete", True, False, "read_summary_and_candidates"),
            ("CancelledResumable", False, True, "resume"),
            ("Failed", False, False, "stop"),
        ]:
            with self.subTest(state=name):
                self.state["scan"].update(self.response(name, done, resume))
                self.assertEqual(self.tool.next_scan_action(self.state), action)

    def test_wire_saved_state_routes_without_reapplying_status(self):
        self.state["scan"].update(self.response("complete", True))
        self.assertEqual(self.tool.next_scan_action(self.state), "read_summary_and_candidates")

    def test_nonterminal_or_unrecognized_state_never_reads_candidates(self):
        for name in ("contract_only", "queued", "validating", "scanning", "aggregating",
                     "evaluating_policy", "auditing", "COMPLETE", " complete", "unknown"):
            with self.subTest(state=name):
                self.tool.apply_scan_status(self.state, self.response(name))
                self.assertEqual(self.tool.next_scan_action(self.state), "status")

    def test_terminal_action_requires_actual_boolean_flag(self):
        for name, field in (("complete", "completed"), ("cancelled_resumable", "resumable"),
                            ("Complete", "completed"), ("CancelledResumable", "resumable")):
            for value in (False, "true", 1, None):
                with self.subTest(state=name, value=value):
                    response = self.response(name)
                    response[field] = value
                    self.tool.apply_scan_status(self.state, response)
                    self.assertEqual(self.tool.next_scan_action(self.state), "status")


if __name__ == "__main__":
    unittest.main()
