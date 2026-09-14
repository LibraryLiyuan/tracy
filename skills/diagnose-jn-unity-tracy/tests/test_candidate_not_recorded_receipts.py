"""A completed search may prove an event was not recorded, never zero CPU work."""
import copy
import tempfile
import unittest
from pathlib import Path

from tests import test_verified_absence_receipts as absence_fixtures
from tests.test_investigation_receipts import load, store
from tests.test_query_native_report_contract import query_native_analysis, load_contract


class CandidateNotRecordedTests(unittest.TestCase):
    def fixture(self, root):
        values = absence_fixtures.VerifiedAbsenceTests().fixture(root)
        c, f, e, req, reply, _, _ = values
        c.update(signature_id="cpu-exact:deserialize", thread_or_queue="thread:main",
                 structural_signature="Main.PlayerLoop > AsyncDeserializeWorld")
        values[6]["result"]["structuredContent"]["data"]["complete"] = True
        req["params"]["arguments"]["params"]["filter"] = {"mode": "exact", "text": "AsyncDeserializeWorld"}
        f["investigation_receipts"][0]["outcome"] = "verified_not_recorded"
        return values

    def validate(self, root, values):
        return absence_fixtures.VerifiedAbsenceTests().validate(root, values)

    def test_exact_named_empty_search_completes_only_unresolved_representative(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.assertEqual([], self.validate(root, self.fixture(root)))

    def test_complete_unfiltered_empty_search_can_also_prove_no_candidate(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); values = self.fixture(root)
            values[3]["params"]["arguments"]["params"].pop("filter")
            self.assertEqual([], self.validate(root, values))

    def set_zones(self, values, zones):
        body = values[4]["result"]["structuredContent"]
        body["data"]["zones"] = zones
        body["page"]["returned"] = len(zones)

    def test_same_name_on_other_thread_does_not_hide_target_event(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); values = self.fixture(root)
            self.set_zones(values, [{"ref": "zone:other", "name": "AsyncDeserializeWorld",
                                    "thread_ref": "thread:loader", "start_ns": "110", "end_ns": "120"}])
            self.assertEqual([], self.validate(root, values))
            values[4]["result"]["structuredContent"]["data"]["zones"][0]["thread_ref"] = "thread:main"
            self.assertTrue(self.validate(root, values))

    def test_real_complete_different_parent_path_excludes_sibling_not_candidate(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); values = self.fixture(root)
            self.set_zones(values, [{"ref": "zone:sibling", "name": "AsyncDeserializeWorld",
                                    "thread_ref": "thread:main", "path": "Loading > AsyncDeserializeWorld",
                                    "start_ns": "110", "end_ns": "120"}])
            self.assertEqual([], self.validate(root, values))
            values[4]["result"]["structuredContent"]["data"]["zones"][0]["path"] = values[0]["structural_signature"]
            self.assertTrue(self.validate(root, values))

    def test_any_matching_or_unresolved_search_node_prevents_absence(self):
        for node in [
            {"ref": "zone:x", "name": "AsyncDeserializeWorld", "thread_ref": "thread:main"},
            {"ref": "zone:x", "name": "AsyncDeserializeWorld"},
            {"ref": "zone:x", "name": "AsyncDeserializeWorld", "thread_ref": "thread:main",
             "path": "Main.PlayerLoop > AsyncDeserializeWorld"},
        ]:
            with self.subTest(node=node), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); values = self.fixture(root); self.set_zones(values, [node])
                self.assertTrue(self.validate(root, values))

    def test_parent_exclusion_requires_real_complete_connected_same_thread_gets(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); values = self.fixture(root)
            self.set_zones(values, [{"ref": "zone:sibling", "name": "AsyncDeserializeWorld",
                "thread_ref": "thread:main", "parent_ref": "zone:parent", "start_ns": "110", "end_ns": "120"}])
            req = copy.deepcopy(values[3]); req["id"] = 3
            req["params"]["arguments"].update(method="zone.cpu.get", params={"ref": "zone:parent"})
            reply = copy.deepcopy(values[4]); reply["id"] = 3
            parent = {"ref": "zone:parent", "parent_ref": None, "name": "Loading", "thread_ref": "thread:main"}
            reply["result"]["structuredContent"]["data"] = parent
            item = {"evidence_id": "PARENT", "request": store(root, "pq.json", req),
                    "response": store(root, "pr.json", reply)}
            values[2]["evidence"].append(item)
            values[1]["investigation_receipts"][0]["exclusion_evidence"] = [
                {"ref": "zone:sibling", "parent_chain_evidence": [{"evidence_id": "PARENT", "entity_json_pointer": "/data"}]}]
            self.assertEqual([], self.validate(root, values))
            for change in ({"name": "Main.PlayerLoop"}, {"parent_ref": "zone:omitted-root"},
                           {"ref": "zone:wrong"}, {"thread_ref": "thread:other"}):
                saved = dict(parent); parent.update(change)
                item["response"] = store(root, "pr.json", reply)
                self.assertTrue(self.validate(root, values))
                parent.clear(); parent.update(saved)

    def test_named_frame_alias_requires_independent_frameset_definition(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); values = self.fixture(root)
            scope = "tracy:v1:aaaaaaaaaaaaaaaa:frame-set:1"
            values[0]["frame_scope"] = scope
            values[6]["result"]["structuredContent"]["data"]["frame_set_ref"] = scope
            self.assertTrue(self.validate(root, values))
            req = copy.deepcopy(values[3]); req["id"] = 4
            req["params"]["arguments"].update(method="frame.sets", params={})
            reply = copy.deepcopy(values[4]); reply["id"] = 4
            reply["result"]["structuredContent"]["data"] = {"frame_sets": [{"ref": scope, "name": "Player.Frame"}]}
            entry = {"evidence_id": "SETS", "request": store(root, "sq.json", req),
                     "response": store(root, "sr.json", reply)}
            values[2]["evidence"].append(entry)
            values[1]["investigation_receipts"][0].update(frame_set_evidence_id="SETS", frame_set_json_pointer="/data/frame_sets/0")
            self.assertEqual([], self.validate(root, values))
            reply["result"]["structuredContent"]["data"]["frame_sets"][0]["name"] = "Other.Frame"
            entry["response"] = store(root, "sr.json", reply)
            self.assertTrue(self.validate(root, values))

    def test_explicit_root_leaf_is_a_complete_different_path_but_missing_parent_is_not(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); values = self.fixture(root)
            node = {"ref": "zone:root-leaf", "name": "AsyncDeserializeWorld", "thread_ref": "thread:main", "parent_ref": None}
            self.set_zones(values, [node])
            values[1]["investigation_receipts"][0]["exclusion_evidence"] = [{"ref": "zone:root-leaf", "parent_chain_evidence": []}]
            self.assertEqual([], self.validate(root, values))
            node.pop("parent_ref")
            self.assertTrue(self.validate(root, values))
            node["parent_ref"] = "zone:unqueried-parent"
            self.assertTrue(self.validate(root, values))
            node["parent_ref"] = None
            values[0]["structural_signature"] = "AsyncDeserializeWorld"
            self.assertTrue(self.validate(root, values))

    def test_wrong_filter_ignored_params_partial_or_missing_frame_cannot_close(self):
        mutations = [
            lambda v: v[3]["params"]["arguments"]["params"].update(filter={"mode": "exact", "text": "Other"}),
            lambda v: v[3]["params"]["arguments"]["params"].update(filter={"mode": "contains", "text": "Async"}),
            lambda v: v[3]["params"]["arguments"]["params"].update(thread_ref="thread:main"),
            lambda v: v[3]["params"]["arguments"]["params"].update(signature_id="cpu-exact:deserialize"),
            lambda v: v[3]["params"]["arguments"]["params"].update(cursor="tail-only"),
            lambda v: v[3]["params"]["arguments"]["params"].update(start_ns="101"),
            lambda v: v[4]["result"]["structuredContent"].update(partial=True),
            lambda v: v[4]["result"]["structuredContent"].update(omitted_count="1"),
            lambda v: v[4]["result"]["structuredContent"]["page"].update(omitted_count_exact=False),
            lambda v: v[4]["result"]["structuredContent"]["page"].update(returned=1),
            lambda v: v[4]["result"]["structuredContent"]["page"].update(next_cursor="next"),
            lambda v: v[4]["result"]["structuredContent"]["budget"].update(exhausted_by=["max_nodes"]),
            lambda v: v[6]["result"]["structuredContent"]["trace"].update(fingerprint="b" * 64),
            lambda v: v[6]["result"]["structuredContent"]["data"].update(complete=False),
            lambda v: v[5]["params"]["arguments"]["params"].update(frame_set="Other.Frame"),
            lambda v: v[5]["params"]["arguments"]["params"].update(index=41),
            lambda v: v[1]["investigation_receipts"][0].pop("frame_anchor_evidence_id"),
        ]
        for i, mutate in enumerate(mutations):
            with self.subTest(case=i), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); values = self.fixture(root); mutate(values)
                self.assertTrue(self.validate(root, values))

    def test_only_specific_cpu_frame_candidate_with_unresolved_conclusion(self):
        mutations = [
            lambda v: v[0].update(domain="gpu"),
            lambda v: v[0].update(domain="job"),
            lambda v: v[0].update(observation_unit="l0_segment"),
            lambda v: v[0].update(signature_id="frame-wall:Player.Frame"),
            lambda v: v[0].update(thread_or_queue=""),
            lambda v: v[1].update(conclusion_status="Supported"),
            lambda v: v[1].update(conclusion_status="Confirmed"),
            lambda v: v[1]["investigation_receipts"][0].update(purpose="sampling"),
            lambda v: v[0]["representative_frames"].append("43"),
        ]
        for i, mutate in enumerate(mutations):
            with self.subTest(case=i), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); values = self.fixture(root); mutate(values)
                self.assertTrue(self.validate(root, values))

    def test_report_requires_this_candidate_and_both_search_and_frame_evidence_in_gap(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            _, _, _, analysis, manifest = query_native_analysis(root)
            values = self.fixture(root)
            c, f, e, *_ = values
            self.assertEqual([], self.validate(root, values))
            c["trigger_evidence"] = manifest["candidates"][0]["trigger_evidence"]
            c["representative_frame_details"] = [{"frame": "42", "reasons": ["first"]}]
            analysis["candidate_findings"][0].update(f, signature_id=c["signature_id"], priority=c["priority"],
                representative_frames=c["representative_frame_details"], evidence_refs=["E1", "FRAME"])
            manifest["candidates"] = [c]
            manifest["policy_algorithm"] = "candidate-policy-v3"
            analysis["bottleneck_signatures"] = []
            analysis["evidence_gaps"] = []
            check = lambda: load_contract().validate_query_native_analysis(analysis, manifest, e, root)
            self.assertTrue(any("absence_gap" in err for err in check()))
            gap = {"candidate_id": c["candidate_id"], "reason": "CandidateNotRecordedInRepresentativeFrame",
                   "evidence_refs": ["E1", "FRAME"], "blocked_by": "Exact event was not recorded in the reference frame.",
                   "minimum_additional_evidence": ["Confirm invocation and coverage in a matching capture."]}
            analysis["evidence_gaps"] = [gap]
            self.assertEqual([], check())
            for field, value in [("candidate_id", "another"), ("evidence_refs", ["E1"]),
                                 ("minimum_additional_evidence", []), ("blocked_by", "")]:
                original = copy.deepcopy(gap[field]); gap[field] = value
                self.assertTrue(any("absence_gap" in err for err in check()))
                gap[field] = original
