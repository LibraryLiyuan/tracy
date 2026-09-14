"""An empty, unfiltered frame query proves missing evidence, never zero cost."""
import copy
import tempfile
import unittest
from pathlib import Path

from tests import test_investigation_receipts as fixtures
from tests.test_investigation_receipts import load, store
from tests.test_query_native_report_contract import query_native_analysis, load_contract


class VerifiedAbsenceTests(unittest.TestCase):
    def fixture(self, root):
        c, f, e, req, reply = fixtures.ReceiptTests().fixture(root)
        c.update(domain="cpu", priority="P1", signature_id="frame-wall:Player.Frame",
                 structural_signature="Player.Frame", thread_or_queue="")
        f["conclusion_status"] = "Unresolved"
        req["params"]["arguments"].update(method="zone.cpu.search", params={
            "start_ns": "100", "end_ns": "200", "limit": 1000, "max_cpu_ms": 30000})
        body = reply["result"]["structuredContent"]
        body.update(data={"zones": []}, partial=False, omitted_count="0",
                    page={"next_cursor": None, "returned": 0, "partial": False,
                          "truncated": False, "omitted_count": "0", "omitted_count_exact": True},
                    budget={"exhausted_by": [], "omitted_count_exact": True})
        anchor_req = copy.deepcopy(req)
        anchor_req["id"] = 2
        anchor_req["params"]["arguments"].update(method="frame.get", params={"frame_set": "Player.Frame", "index": 42})
        anchor = {"jsonrpc": "2.0", "id": 2, "result": {"structuredContent": {
            "ok": True, "trace": {"fingerprint": "a" * 64}, "data": {
                "frame_set_ref": "Player.Frame", "index": "42", "begin_ns": "100", "end_ns": "200"}}}}
        e["evidence"].append({"evidence_id": "FRAME", "request": store(root, "fq.json", anchor_req),
                              "response": store(root, "fr.json", anchor)})
        f["investigation_receipts"][0].update(outcome="verified_absent", entity_json_pointers=[],
            frame_anchor_evidence_id="FRAME", frame_anchor_json_pointer="/data")
        return c, f, e, req, reply, anchor_req, anchor

    def validate(self, root, values):
        c, f, e, req, reply, aq, ar = values
        e["evidence"][0].update(request=store(root, "request.json", req), response=store(root, "response.json", reply))
        e["evidence"][1].update(request=store(root, "fq.json", aq), response=store(root, "fr.json", ar))
        return load().validate_receipts(c, f, e, root)

    def test_complete_unfiltered_empty_frame_is_investigated_but_unresolved(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.assertEqual([], self.validate(root, self.fixture(root)))

    def test_named_frame_request_must_resolve_to_the_candidates_opaque_frame_set(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); values = self.fixture(root)
            scope = "tracy:v1:aaaaaaaaaaaaaaaa:frame-set:0"
            values[0].update(frame_scope=scope, signature_id="frame-wall:" + scope)
            values[6]["result"]["structuredContent"]["data"]["frame_set_ref"] = scope
            self.assertEqual([], self.validate(root, values))
            values[6]["result"]["structuredContent"]["data"]["frame_set_ref"] = scope + "1"
            self.assertTrue(self.validate(root, values))

    def test_malformed_budget_is_rejected_instead_of_crashing_validation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); values = self.fixture(root)
            values[4]["result"]["structuredContent"]["budget"] = None
            self.assertTrue(self.validate(root, values))

    def test_filters_cursors_omissions_budget_and_nonempty_results_cannot_prove_absence(self):
        mutations = [
            lambda v: v[3]["params"]["arguments"]["params"].update(text="Main.PlayerLoop"),
            lambda v: v[3]["params"]["arguments"]["params"].update(thread_ref="thread:main"),
            lambda v: v[3]["params"]["arguments"]["params"].update(cursor="page-2"),
            lambda v: v[4]["result"]["structuredContent"]["page"].update(next_cursor="page-2"),
            lambda v: v[4]["result"]["structuredContent"]["page"].update(omitted_count="1"),
            lambda v: v[4]["result"]["structuredContent"]["page"].update(omitted_count_exact=False),
            lambda v: v[4]["result"]["structuredContent"]["budget"].update(exhausted_by=["max_cpu_ms"]),
            lambda v: v[4]["result"]["structuredContent"].update(omitted_count="1"),
            lambda v: v[4]["result"]["structuredContent"]["data"].update(zones=[{"ref": "zone:real"}]),
            lambda v: v[4]["result"]["structuredContent"].pop("page"),
            lambda v: v[4]["result"]["structuredContent"].update(partial=True),
        ]
        for i, mutate in enumerate(mutations):
            with self.subTest(case=i), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); values = self.fixture(root); mutate(values)
                self.assertTrue(self.validate(root, values))

    def test_exact_frame_identity_and_boundaries_are_required(self):
        mutations = [
            lambda v: v[3]["params"]["arguments"]["params"].update(start_ns="101"),
            lambda v: v[3]["params"]["arguments"]["params"].update(end_ns="201"),
            lambda v: v[5]["params"]["arguments"]["params"].update(index=41),
            lambda v: v[5]["params"]["arguments"]["params"].update(frame_set="Other.Frame"),
            lambda v: v[6]["result"]["structuredContent"]["data"].update(index="41"),
            lambda v: v[6]["result"]["structuredContent"]["trace"].update(fingerprint="b" * 64),
            lambda v: v[1]["investigation_receipts"][0].pop("frame_anchor_evidence_id"),
        ]
        for i, mutate in enumerate(mutations):
            with self.subTest(case=i), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); values = self.fixture(root); mutate(values)
                self.assertTrue(self.validate(root, values))

    def test_zone_gpu_or_positive_conclusion_cannot_use_absence_shortcut(self):
        mutations = [
            lambda v: v[0].update(signature_id="exact:work", frame_root=True),
            lambda v: v[0].update(domain="gpu"),
            lambda v: v[0].update(observation_unit="l0_segment"),
            lambda v: v[1].update(conclusion_status="Supported"),
            lambda v: v[1].update(conclusion_status="Confirmed"),
            lambda v: v[1]["investigation_receipts"][0].update(outcome="invented_gap"),
        ]
        for i, mutate in enumerate(mutations):
            with self.subTest(case=i), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); values = self.fixture(root); mutate(values)
                self.assertTrue(self.validate(root, values))

    def test_each_representative_frame_still_needs_its_own_query(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); values = self.fixture(root)
            values[0]["representative_frames"].append("43")
            self.assertTrue(self.validate(root, values))

    def test_report_requires_candidate_linked_absence_gap_with_minimum_evidence(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            _, _, _, analysis, manifest = query_native_analysis(root)
            values = self.fixture(root)
            c, f, e, *_ = values
            self.assertEqual([], self.validate(root, values))
            c["trigger_evidence"] = manifest["candidates"][0]["trigger_evidence"]
            c["representative_frame_details"] = [{"frame": "42", "reasons": ["first"]}]
            # Reuse a complete narrative fixture; numeric facts still come from Query.
            full_finding = analysis["candidate_findings"][0]
            full_finding.update(f, signature_id=c["signature_id"], priority="P1",
                representative_frames=c["representative_frame_details"], evidence_refs=["E1", "FRAME"])
            manifest["candidates"] = [c]
            manifest["policy_algorithm"] = "candidate-policy-v3"
            analysis["bottleneck_signatures"] = []
            analysis["evidence_gaps"] = []
            validate = lambda: load_contract().validate_query_native_analysis(analysis, manifest, e, root)
            self.assertTrue(any("absence_gap" in error for error in validate()))
            analysis["evidence_gaps"] = [{"candidate_id": c["candidate_id"],
                "reason": "NoRecordedCpuZonesInRepresentativeFrame", "evidence_refs": ["E1", "FRAME"],
                "blocked_by": "Frame timing exists, but the complete interval has no recorded CPU Zone.",
                "minimum_additional_evidence": ["CPU instrumentation covering this interval in a new capture."]}]
            self.assertEqual([], validate())
            analysis["evidence_gaps"][0]["candidate_id"] = "another-candidate"
            self.assertTrue(any("absence_gap" in error for error in validate()))
