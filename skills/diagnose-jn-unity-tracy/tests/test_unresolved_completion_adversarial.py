"""Malformed raw data must be rejected, and open ancestors prove structure only."""
import copy
import tempfile
import unittest
from pathlib import Path

from tests import test_candidate_not_recorded_receipts as candidate_cases
from tests import test_unresolved_completion_receipts as wait_cases
from tests.test_investigation_receipts import store


class UnresolvedCompletionAdversarialTests(unittest.TestCase):
    def assert_diagnostic_rejection(self, invoke):
        try:
            errors = invoke()
        except Exception as exc:
            self.fail(f"Malformed saved evidence crashed validation: {type(exc).__name__}: {exc}")
        self.assertIsInstance(errors, list)
        self.assertTrue(errors, "Malformed evidence was accepted")
        self.assertTrue(all(isinstance(error, str) and error for error in errors))

    def test_non_object_candidate_frame_anchor_is_diagnosed_not_crashed(self):
        for data in (None, [], "not-frame-data", 42):
            with self.subTest(data=data), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); case = candidate_cases.CandidateNotRecordedTests(); values = case.fixture(root)
                values[6]["result"]["structuredContent"]["data"] = data
                self.assert_diagnostic_rejection(lambda: case.validate(root, values))

    def test_non_object_cpu_search_collection_is_diagnosed_not_crashed(self):
        for data in (None, [], "not-zone-data", {"zones": None}, {"zones": {}}, {"zones": [None]}):
            with self.subTest(data=data), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); case = candidate_cases.CandidateNotRecordedTests(); values = case.fixture(root)
                values[4]["result"]["structuredContent"]["data"] = data
                self.assert_diagnostic_rejection(lambda: case.validate(root, values))

    def test_non_object_relation_collection_is_diagnosed_not_crashed(self):
        for eid in ("OUT", "IN"):
            for data in (None, [], "not-relation-data", {"relations": None}, {"relations": {}}, {"relations": [None]}):
                with self.subTest(eid=eid, data=data), tempfile.TemporaryDirectory() as temp:
                    root = Path(temp); case = wait_cases.CompletionNotRecordedReceiptTests(); values = case.fixture()
                    values["replies"][eid]["result"]["structuredContent"]["data"] = data
                    self.assert_diagnostic_rejection(lambda: case.validate(root, values))

    def test_ref_tree_ignored_frame_parameters_cannot_fabricate_an_extra_representative(self):
        # zone.cpu.tree is ref-based. Query ignores frame_index/frame_scope;
        # retaining Frame42's actual tree cannot also establish Frame43 merely
        # by adding those unsupported parameters to that same request.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); case = wait_cases.CompletionNotRecordedReceiptTests(); values = case.fixture()
            values["finding"]["investigation_receipts"].pop(1)  # Remove the actual Frame43 tree.
            values["requests"]["TREE42"]["params"]["arguments"]["params"].update(
                frame_index=43, frame_scope=values["candidate"]["frame_scope"])
            self.assert_diagnostic_rejection(lambda: case.validate(root, values))

    def alias_fixture(self, root):
        case = candidate_cases.CandidateNotRecordedTests(); values = case.fixture(root)
        scope = "tracy:v1:aaaaaaaaaaaaaaaa:frame-set:1"
        values[0]["frame_scope"] = scope
        values[6]["result"]["structuredContent"]["data"]["frame_set_ref"] = scope
        request = copy.deepcopy(values[3]); request["id"] = 8
        request["params"]["arguments"].update(method="frame.sets", params={})
        reply = copy.deepcopy(values[4]); reply["id"] = 8
        reply["result"]["structuredContent"]["data"] = {"frame_sets": [{"ref": scope, "name": "Player.Frame"}]}
        reply["result"]["structuredContent"]["page"]["returned"] = 1
        entry = dict(evidence_id="SETS", request=store(root, "sets-q.json", request),
                     response=store(root, "sets-r.json", reply))
        values[2]["evidence"].append(entry)
        values[1]["investigation_receipts"][0].update(frame_set_evidence_id="SETS", frame_set_json_pointer="/data/frame_sets/0")
        return case, values, entry, reply

    def test_real_shaped_frame_set_alias_fixture_still_passes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); case, values, _, _ = self.alias_fixture(root)
            self.assertEqual([], case.validate(root, values))

    def test_non_object_frame_set_data_or_selected_element_is_diagnosed_not_crashed(self):
        for data, pointer in ((None, "/data"), ([], "/data"),
                              ({"frame_sets": [None]}, "/data/frame_sets/0"),
                              ({"frame_sets": ["bad"]}, "/data/frame_sets/0")):
            with self.subTest(data=data, pointer=pointer), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); case, values, entry, reply = self.alias_fixture(root)
                reply["result"]["structuredContent"]["data"] = data
                entry["response"] = store(root, "sets-r.json", reply)
                values[1]["investigation_receipts"][0]["frame_set_json_pointer"] = pointer
                self.assert_diagnostic_rejection(lambda: case.validate(root, values))

    def paged_fixture(self, root):
        case = candidate_cases.CandidateNotRecordedTests(); values = case.fixture(root)
        first = values[4]["result"]["structuredContent"]
        case.set_zones(values, [dict(ref="zone:other", name="AsyncDeserializeWorld", thread_ref="thread:other")])
        first["page"]["next_cursor"] = "real-next-page"
        request = copy.deepcopy(values[3]); request["id"] = 10
        request["params"]["arguments"]["params"]["cursor"] = "real-next-page"
        reply = copy.deepcopy(values[4]); reply["id"] = 10
        reply["result"]["structuredContent"]["data"] = {"zones": []}
        reply["result"]["structuredContent"]["page"].update(returned=0, next_cursor=None)
        entry = dict(evidence_id="PAGE2", request=store(root, "page2-q.json", request),
                     response=store(root, "page2-r.json", reply))
        values[2]["evidence"].append(entry)
        values[1]["investigation_receipts"][0]["page_evidence_ids"] = ["E1", "PAGE2"]
        return case, values, entry, reply

    def test_complete_original_page_chain_remains_acceptable(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); case, values, _, _ = self.paged_fixture(root)
            self.assertEqual([], case.validate(root, values))

    def test_non_object_collection_on_later_page_is_diagnosed_not_crashed(self):
        for data in (None, [], "not-a-page-object"):
            with self.subTest(data=data), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); case, values, entry, reply = self.paged_fixture(root)
                reply["result"]["structuredContent"]["data"] = data
                entry["response"] = store(root, "page2-r.json", reply)
                self.assert_diagnostic_rejection(lambda: case.validate(root, values))

    def exclusion_fixture(self, root):
        case = candidate_cases.CandidateNotRecordedTests(); values = case.fixture(root)
        case.set_zones(values, [dict(ref="zone:sibling", name="AsyncDeserializeWorld",
            thread_ref="thread:main", parent_ref="zone:open-parent", start_ns="110", end_ns="120", complete=True)])
        request = copy.deepcopy(values[3]); request["id"] = 11
        request["params"]["arguments"].update(method="zone.cpu.get", params={"ref": "zone:open-parent"})
        reply = copy.deepcopy(values[4]); reply["id"] = 11
        parent = dict(ref="zone:open-parent", name="Loading", parent_ref=None, thread_ref="thread:main",
                      start_ns="50", end_ns=None, duration_ns=None, complete=False)
        reply["result"]["structuredContent"]["data"] = parent
        entry = dict(evidence_id="OPEN_PARENT", request=store(root, "open-parent-q.json", request),
                     response=store(root, "open-parent-r.json", reply))
        values[2]["evidence"].append(entry)
        values[1]["investigation_receipts"][0]["exclusion_evidence"] = [dict(ref="zone:sibling",
            parent_chain_evidence=[dict(evidence_id="OPEN_PARENT", entity_json_pointer="/data")])]
        return case, values, entry, reply, parent

    def test_open_ancestor_proves_only_connected_structure_without_replacing_missing_duration(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); case, values, _, reply, parent = self.exclusion_fixture(root)
            original = copy.deepcopy(parent)
            self.assertEqual([], case.validate(root, values))
            self.assertEqual(original, parent)
            self.assertIsNone(reply["result"]["structuredContent"]["data"]["end_ns"])
            self.assertIsNone(reply["result"]["structuredContent"]["data"]["duration_ns"])

    def test_open_ancestor_does_not_hide_the_actual_candidate_path(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); case, values, entry, reply, parent = self.exclusion_fixture(root)
            parent["name"] = "Main.PlayerLoop"
            entry["response"] = store(root, "open-parent-r.json", reply)
            self.assert_diagnostic_rejection(lambda: case.validate(root, values))

    def test_positive_conclusion_wrong_frame_or_partial_cannot_use_exclusion_gap(self):
        for mutation in ("Supported", "Confirmed", "wrong_frame", "partial_search", "partial_parent"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); case, values, entry, reply, _ = self.exclusion_fixture(root)
                if mutation in ("Supported", "Confirmed"): values[1]["conclusion_status"] = mutation
                elif mutation == "wrong_frame": values[5]["params"]["arguments"]["params"]["index"] = 41
                elif mutation == "partial_search": values[4]["result"]["structuredContent"]["partial"] = True
                elif mutation == "partial_parent":
                    reply["result"]["structuredContent"]["partial"] = True
                    entry["response"] = store(root, "open-parent-r.json", reply)
                self.assert_diagnostic_rejection(lambda: case.validate(root, values))


if __name__ == "__main__":
    unittest.main()
