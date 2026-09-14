"""A complete empty CPU-relation domain can bound an Unresolved wait inquiry.

The production validator reads real JSON-RPC files and hashes. These small
synthetic files mirror the saved relation.search shape, not a mocked validator.
Missing entities, filtered empty results, and unsupported conclusions must fail.
"""
import copy
import tempfile
import unittest
from pathlib import Path

from tests.test_investigation_receipts import load, store
from tests.test_query_native_report_contract import query_native_analysis, load_contract


class CompletionNotRecordedReceiptTests(unittest.TestCase):
    def fixture(self):
        scope = "tracy:v1:aaaaaaaaaaaaaaaa:frame-set:0"
        thread = "tracy:v1:aaaaaaaaaaaaaaaa:thread:10b6c"
        candidate = dict(candidate_id="candidate:wait", domain="cpu", selected=True,
            priority="P1", metric="wait", signature_id="cpu-exact:wait",
            structural_signature="Main.PlayerLoop > NativeWait", observation_unit="frame",
            frame_scope=scope, thread_or_queue=thread, manifestation="frame_cost",
            representative_frames=["42", "43"], representative_frame_details=[
                {"frame": "42", "reasons": ["worst"]},
                {"frame": "43", "reasons": ["normal"]}])
        trace = dict(id="trace-1", fingerprint="a" * 64, revision="0", complete=True,
                     source_kind="snapshot", state="ready", watermark_ns="1000")
        page = dict(limit=1000, returned=0, next_cursor=None, partial=False,
                    truncated=False, omitted_count="0", omitted_count_exact=True)
        budget = dict(exhausted_by=[], omitted_count_exact=True,
            consumed=dict(cpu_ms="10", scan_events="3198213", nodes="0", edges="0", groups="0"),
            limits=dict(max_cpu_ms="30000", max_scan_events="50000000", max_nodes="10000",
                        max_edges="20000", max_groups="500"))
        requests, replies = {}, {}

        def add(eid, request_id, method, params, data):
            requests[eid] = dict(jsonrpc="2.0", id=request_id, method="tools/call",
                params=dict(name="tracy_inspect", arguments=dict(trace_id="trace-1", method=method, params=params)))
            replies[eid] = dict(jsonrpc="2.0", id=request_id, result=dict(structuredContent=dict(
                protocol="tracy-query/1", schema_version="1.35.0", ok=True, trace=copy.deepcopy(trace),
                partial=False, omitted_count="0", warnings=[], data=data,
                page=copy.deepcopy(page), budget=copy.deepcopy(budget))))

        parent = dict(ref="zone:parent", name="Main.PlayerLoop", parent_ref=None,
                      thread_ref=thread, start_ns="100", end_ns="300", complete=True)
        add("PARENT", 1, "zone.cpu.get", {"ref": "zone:parent"}, parent)
        receipts = []
        for frame, begin, end, request_id in (("42", "100", "200", 2), ("43", "200", "300", 4)):
            zone = dict(ref="zone:wait" + frame, name="NativeWait", parent_ref="zone:parent",
                thread_ref=thread, start_ns=str(int(begin) + 10), end_ns=str(int(end) - 10),
                complete=True, timing_valid=True, child_count=0, provenance="SiteReused")
            add("TREE" + frame, request_id, "zone.cpu.tree", {"ref": zone["ref"], "limit": 1000},
                {"root": zone, "children": []})
            add("FRAME" + frame, request_id + 1, "frame.get", {"frame_set": scope, "index": int(frame)},
                dict(frame_set_ref=scope, index=int(frame), ref="frame:" + frame,
                     begin_ns=begin, end_ns=end, complete=True))
            receipts.append(dict(evidence_id="TREE" + frame, purpose="zone_tree", manifestation="frame_cost",
                entity_json_pointers=["/data/root"], frame_anchor_evidence_id="FRAME" + frame,
                frame_anchor_json_pointer="/data", parent_chain_evidence=[
                    dict(evidence_id="PARENT", entity_json_pointer="/data")]))
        for eid, direction, request_id in (("OUT", "source_kind", 6), ("IN", "target_kind", 7)):
            add(eid, request_id, "relation.search",
                {direction: "cpu_zone", "limit": 1000, "max_cpu_ms": 30000, "max_scan_events": 50000000},
                dict(present=True, complete=True, relation_schema_version=1,
                     relation_count="3198213", matched_count="0", relations=[],
                     provenance="exact-binary", reason="", trust="untrusted_trace_data"))
        receipts.append(dict(evidence_id="OUT", purpose="wait_completion", manifestation="frame_cost",
            outcome="completion_not_recorded", relation_evidence_ids=["OUT", "IN"], entity_json_pointers=[]))
        finding = dict(candidate_id=candidate["candidate_id"], conclusion_status="Unresolved",
                       investigation_receipts=receipts)
        return dict(candidate=candidate, finding=finding, requests=requests, replies=replies)

    def evidence(self, root, values):
        return dict(trace=dict(trace_id="trace-1", trace_sha256="a" * 64), evidence=[
            dict(evidence_id=eid, request=store(root, eid + "-request.json", req),
                 response=store(root, eid + "-response.json", values["replies"][eid]))
            for eid, req in values["requests"].items()])

    def validate(self, root, values):
        return load().validate_receipts(values["candidate"], values["finding"], self.evidence(root, values), root)

    def test_complete_empty_relation_pair_and_all_real_wait_representatives_can_close_unresolved(self):
        with tempfile.TemporaryDirectory() as temp:
            self.assertEqual([], self.validate(Path(temp), self.fixture()))

    def test_relation_pair_does_not_supply_missing_zone_representative_coverage(self):
        for mutation in ("missing_one_tree", "no_tree", "missing_frame_anchor", "wrong_frame_set",
                         "wrong_returned_frame", "wrong_requested_frame", "outside_frame",
                         "wrong_thread", "wrong_path", "wrong_parent", "partial_tree"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                v = self.fixture(); f = v["finding"]; c = v["candidate"]
                node = v["replies"]["TREE42"]["result"]["structuredContent"]["data"]["root"]
                frame = v["replies"]["FRAME42"]["result"]["structuredContent"]["data"]
                if mutation == "missing_one_tree": f["investigation_receipts"].pop(0)
                elif mutation == "no_tree": f["investigation_receipts"] = f["investigation_receipts"][-1:]
                elif mutation == "missing_frame_anchor": f["investigation_receipts"][0].pop("frame_anchor_evidence_id")
                elif mutation == "wrong_frame_set": frame["frame_set_ref"] = "Other.Frame"
                elif mutation == "wrong_returned_frame": frame["index"] = 99
                elif mutation == "wrong_requested_frame": v["requests"]["FRAME42"]["params"]["arguments"]["params"]["index"] = 99
                elif mutation == "outside_frame": node.update(start_ns="300", end_ns="400")
                elif mutation == "wrong_thread": node["thread_ref"] = "thread:other"
                elif mutation == "wrong_path": c["structural_signature"] = "OtherParent > NativeWait"
                elif mutation == "wrong_parent": node["parent_ref"] = "zone:unrelated"
                elif mutation == "partial_tree": v["replies"]["TREE42"]["result"]["structuredContent"]["partial"] = True
                self.assertTrue(self.validate(Path(temp), v))

    def test_relation_domain_must_be_unfiltered_and_the_kind_token_exact(self):
        extras = [
            {"namespace": "gfx"}, {"relation": "continues_as"},
            {"filter": {"mode": "exact", "text": "missing"}},
            {"thread_ref": "tracy:v1:aaaaaaaaaaaaaaaa:thread:10b6c"},
            {"start_ns": "100", "end_ns": "200"}, {"cursor": "page-2"},
            {"unknown_filter": "ignored"}, {"fields": ["relations"]}]
        for eid in ("OUT", "IN"):
            for extra in extras:
                with self.subTest(eid=eid, extra=extra), tempfile.TemporaryDirectory() as temp:
                    v = self.fixture(); v["requests"][eid]["params"]["arguments"]["params"].update(extra)
                    self.assertTrue(self.validate(Path(temp), v))
            for token in ("cpu-zone", "CpuZone", "not_a_kind", "gpu_pass", ""):
                with self.subTest(eid=eid, token=token), tempfile.TemporaryDirectory() as temp:
                    v = self.fixture(); key = "source_kind" if eid == "OUT" else "target_kind"
                    v["requests"][eid]["params"]["arguments"]["params"][key] = token
                    self.assertTrue(self.validate(Path(temp), v))

    def test_both_directions_are_required_and_cannot_be_narrowed_to_their_intersection(self):
        for mutation in ("single", "duplicate", "missing", "out_as_in", "in_as_out", "both_kind_filters", "wrong_evidence_id"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                v = self.fixture(); receipt = v["finding"]["investigation_receipts"][-1]
                if mutation == "single": receipt["relation_evidence_ids"] = ["OUT"]
                elif mutation == "duplicate": receipt["relation_evidence_ids"] = ["OUT", "OUT"]
                elif mutation == "missing": receipt.pop("relation_evidence_ids")
                elif mutation == "out_as_in": v["requests"]["IN"]["params"]["arguments"]["params"] = dict(source_kind="cpu_zone")
                elif mutation == "in_as_out": v["requests"]["OUT"]["params"]["arguments"]["params"] = dict(target_kind="cpu_zone")
                elif mutation == "both_kind_filters": v["requests"]["OUT"]["params"]["arguments"]["params"]["target_kind"] = "cpu_zone"
                elif mutation == "wrong_evidence_id": receipt["evidence_id"] = "PARENT"
                self.assertTrue(self.validate(Path(temp), v))

    def test_relation_pair_requires_same_trace_revision_and_request_response_identity(self):
        for eid in ("OUT", "IN"):
            for mutation in ("trace_id", "fingerprint", "revision", "request_id", "wrong_method", "failed"):
                with self.subTest(eid=eid, mutation=mutation), tempfile.TemporaryDirectory() as temp:
                    v = self.fixture(); args = v["requests"][eid]["params"]["arguments"]
                    body = v["replies"][eid]["result"]["structuredContent"]
                    if mutation == "trace_id": args["trace_id"] = "trace-other"
                    elif mutation == "fingerprint": body["trace"]["fingerprint"] = "b" * 64
                    elif mutation == "revision": body["trace"]["revision"] = "1"
                    elif mutation == "request_id": v["replies"][eid]["id"] = 999
                    elif mutation == "wrong_method": args["method"] = "relation.get"
                    elif mutation == "failed": body.update(ok=False, error={"code": "INVALID_PARAMS"})
                    self.assertTrue(self.validate(Path(temp), v))

    def test_partial_omitted_unfinished_or_positive_relation_results_cannot_prove_absence(self):
        mutations = ("partial", "truncated", "budget", "outer_omitted", "omitted_inexact", "next_cursor",
                     "page_partial", "page_truncated", "page_omitted", "page_inexact", "missing_page",
                     "nested_has_more", "not_present", "not_complete", "positive_matched", "positive_relations",
                     "positive_returned", "heuristic", "missing_matched", "malformed_budget")
        for eid in ("OUT", "IN"):
            for mutation in mutations:
                with self.subTest(eid=eid, mutation=mutation), tempfile.TemporaryDirectory() as temp:
                    v = self.fixture(); b = v["replies"][eid]["result"]["structuredContent"]
                    if mutation == "partial": b["partial"] = True
                    elif mutation == "truncated": b["truncated"] = True
                    elif mutation == "budget": b["budget"]["exhausted_by"] = ["max_scan_events"]
                    elif mutation == "outer_omitted": b["omitted_count"] = "1"
                    elif mutation == "omitted_inexact": b["budget"]["omitted_count_exact"] = False
                    elif mutation == "next_cursor": b["page"]["next_cursor"] = "page-2"
                    elif mutation == "page_partial": b["page"]["partial"] = True
                    elif mutation == "page_truncated": b["page"]["truncated"] = True
                    elif mutation == "page_omitted": b["page"]["omitted_count"] = "1"
                    elif mutation == "page_inexact": b["page"]["omitted_count_exact"] = False
                    elif mutation == "missing_page": b.pop("page")
                    elif mutation == "nested_has_more": b["data"]["pagination"] = {"has_more": True}
                    elif mutation == "not_present": b["data"]["present"] = False
                    elif mutation == "not_complete": b["data"]["complete"] = False
                    elif mutation == "positive_matched": b["data"]["matched_count"] = "1"
                    elif mutation == "positive_relations": b["data"]["relations"] = [{"source_kind": "cpu_zone", "source_kind_id": 2}]
                    elif mutation == "positive_returned": b["page"]["returned"] = 1
                    elif mutation == "heuristic": b["data"]["provenance"] = "heuristic"
                    elif mutation == "missing_matched": b["data"].pop("matched_count")
                    elif mutation == "malformed_budget": b["budget"] = None
                    self.assertTrue(self.validate(Path(temp), v))

    def test_only_cpu_wait_unresolved_can_use_completion_not_recorded(self):
        mutations = [{"conclusion_status": "Supported"}, {"conclusion_status": "Confirmed"},
                     {"conclusion_status": "Rejected"}]
        for mutation in mutations:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                v = self.fixture(); v["finding"].update(mutation)
                self.assertTrue(self.validate(Path(temp), v))
        for mutation in ({"domain": "gpu"}, {"domain": "job"}, {"metric": "exclusive"}):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                v = self.fixture(); v["candidate"].update(mutation)
                self.assertTrue(self.validate(Path(temp), v))


class CompletionNotRecordedReportTests(unittest.TestCase):
    def fixture(self, root):
        case = CompletionNotRecordedReceiptTests()
        values = case.fixture()
        _, _, _, analysis, manifest = query_native_analysis(root)
        candidate = values["candidate"]
        candidate["trigger_evidence"] = manifest["candidates"][0]["trigger_evidence"]
        evidence = case.evidence(root, values)
        finding = analysis["candidate_findings"][0]
        finding.update(values["finding"], signature_id=candidate["signature_id"], priority="P1",
            representative_frames=candidate["representative_frame_details"],
            evidence_refs=[e["evidence_id"] for e in evidence["evidence"]])
        manifest["candidates"] = [candidate]
        manifest["policy_algorithm"] = "candidate-policy-v3"
        analysis["bottleneck_signatures"] = []
        analysis["evidence_gaps"] = [dict(candidate_id=candidate["candidate_id"],
            reason="UnresolvedWaitChain", evidence_refs=["OUT", "IN"],
            blocked_by="This capture records no CPU Zone relations; the completion end cannot be identified.",
            minimum_additional_evidence=["Capture the typed Wait-to-completion and continuation relation."])]
        return analysis, manifest, evidence

    def test_unresolved_completion_gap_is_candidate_bound_and_preserves_both_original_relation_receipts(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, manifest, evidence = self.fixture(root)
            self.assertEqual([], load_contract().validate_query_native_analysis(analysis, manifest, evidence, root))

    def test_missing_wrong_candidate_or_incomplete_gap_cannot_close_wait(self):
        for mutation in ("no_gap", "wrong_candidate", "wrong_reason", "missing_outgoing", "missing_incoming",
                         "missing_blocker", "empty_blocker", "missing_minimum", "empty_minimum"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); analysis, manifest, evidence = self.fixture(root)
                gap = analysis["evidence_gaps"][0]
                if mutation == "no_gap": analysis["evidence_gaps"] = []
                elif mutation == "wrong_candidate": gap["candidate_id"] = "candidate:other"
                elif mutation == "wrong_reason": gap["reason"] = "NoRecordedCpuZonesInRepresentativeFrame"
                elif mutation == "missing_outgoing": gap["evidence_refs"] = ["IN"]
                elif mutation == "missing_incoming": gap["evidence_refs"] = ["OUT"]
                elif mutation == "missing_blocker": gap.pop("blocked_by")
                elif mutation == "empty_blocker": gap["blocked_by"] = ""
                elif mutation == "missing_minimum": gap.pop("minimum_additional_evidence")
                elif mutation == "empty_minimum": gap["minimum_additional_evidence"] = []
                self.assertTrue(load_contract().validate_query_native_analysis(analysis, manifest, evidence, root))


if __name__ == "__main__":
    unittest.main()
