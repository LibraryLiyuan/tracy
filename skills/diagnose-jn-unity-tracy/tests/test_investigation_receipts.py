import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load():
    spec = importlib.util.spec_from_file_location("receipts", ROOT / "scripts" / "investigation_receipts.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def store(root, name, data):
    payload = json.dumps(data).encode()
    (root / name).write_bytes(payload)
    return {"path": name, "sha256": hashlib.sha256(payload).hexdigest()}


class ReceiptTests(unittest.TestCase):
    def fixture(self, root):
        candidate = {"candidate_id": "candidate:1", "signature_id": "exact:work", "selected": True,
                     "structural_signature": "Parent > Work", "frame_scope": "Player.Frame",
                     "thread_or_queue": "thread:main", "observation_unit": "frame",
                     "manifestation": "frame_cost", "metric": "exclusive", "representative_frames": ["42"]}
        request = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
            "name": "tracy_inspect", "arguments": {"trace_id": "trace-1", "method": "zone.cpu.tree",
                "params": {"frame_index": 42, "frame_scope": "Player.Frame", "thread_ref": "thread:main"}}}}
        response = {"jsonrpc": "2.0", "id": 1, "result": {"structuredContent": {"ok": True,
            "trace": {"fingerprint": "a" * 64},
            "data": {"nodes": [{"ref": "zone:work", "name": "Work", "path": "Parent > Work", "thread_ref": "thread:main"}]}}}}
        item = {"evidence_id": "E1", "request": store(root, "request.json", request),
                "response": store(root, "response.json", response)}
        finding = {"candidate_id": "candidate:1", "conclusion_status": "Supported",
                   "investigation_receipts": [{"evidence_id": "E1", "purpose": "zone_tree",
                       "entity_json_pointers": ["/data/nodes/0"], "manifestation": "frame_cost"}]}
        return candidate, finding, {"trace": {"trace_id": "trace-1", "trace_sha256": "a" * 64}, "evidence": [item]}, request, response

    def test_real_scoped_reply_passes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            c, f, e, _, _ = self.fixture(root)
            self.assertEqual([], load().validate_receipts(c, f, e, root))

    def test_prose_without_request_is_not_investigation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            c, f, e, _, _ = self.fixture(root)
            f["investigation_receipts"] = []
            self.assertTrue(load().validate_receipts(c, f, e, root))

    def test_same_mcp_trace_id_from_another_capture_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            c, f, e, _, reply = self.fixture(root)
            reply["result"]["structuredContent"]["trace"]["fingerprint"] = "b" * 64
            e["evidence"][0]["response"] = store(root, "response.json", reply)
            self.assertTrue(load().validate_receipts(c, f, e, root))

    def test_frame_boundary_is_not_a_synthetic_zone_name(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            c, f, e, req, reply = self.fixture(root)
            c.update(frame_root=True, structural_signature="Player.Frame", signature_id="frame-wall:Player.Frame")
            node = reply["result"]["structuredContent"]["data"]["nodes"][0]
            node.update(name="Main.PlayerLoop", path="Main.PlayerLoop", parent_ref=None, complete=True)
            e["evidence"][0]["response"] = store(root, "response.json", reply)
            self.assertEqual([], load().validate_receipts(c, f, e, root))
            node["parent_ref"] = "zone:parent"
            e["evidence"][0]["response"] = store(root, "response.json", reply)
            self.assertTrue(load().validate_receipts(c, f, e, root))

    def test_wrong_thread_or_frame_or_signature_cannot_close(self):
        for field, value in [("thread_ref", "thread:other"), ("frame_index", 41)]:
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                c, f, e, req, _ = self.fixture(root)
                req["params"]["arguments"]["params"][field] = value
                e["evidence"][0]["request"] = store(root, "request.json", req)
                self.assertTrue(load().validate_receipts(c, f, e, root))
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            c, f, e, _, reply = self.fixture(root)
            reply["result"]["structuredContent"]["data"]["nodes"][0]["name"] = "UnrelatedWork"
            e["evidence"][0]["response"] = store(root, "response.json", reply)
            self.assertTrue(load().validate_receipts(c, f, e, root))

    def test_corruption_partial_and_unsupported_claim_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            c, f, e, _, response = self.fixture(root)
            (root / "request.json").write_text("{}")
            self.assertTrue(load().validate_receipts(c, f, e, root))
            c, f, e, _, response = self.fixture(root)
            response["result"]["structuredContent"]["partial"] = True
            e["evidence"][0]["response"] = store(root, "response.json", response)
            self.assertTrue(load().validate_receipts(c, f, e, root))

    def test_gpu_segments_require_time_receipts_not_fake_frames(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            c, f, e, req, reply = self.fixture(root)
            c.update(observation_unit="l0_segment", representative_frames=[], representative_intervals=[{
                "begin_ns": "100", "end_ns": "200", "l0_segment_ordinal": "7"}])
            self.assertTrue(load().validate_receipts(c, f, e, root))

    def test_actual_query_page_contract_cannot_hide_unread_pages(self):
        # Query 1.35 uses 'page', not 'pagination'; global partial can be false
        # while a successful search still has an unread next page.
        for page in ({"next_cursor": "next-page", "partial": False, "truncated": False},
                     {"next_cursor": None, "partial": True, "truncated": False},
                     {"next_cursor": None, "partial": False, "truncated": True}):
            with self.subTest(page=page), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                c, f, e, _, reply = self.fixture(root)
                reply["result"]["structuredContent"].update(partial=False, page=page)
                e["evidence"][0]["response"] = store(root, "response.json", reply)
                self.assertTrue(load().validate_receipts(c, f, e, root))

    def test_ref_tree_uses_actual_zone_interval_and_frame_anchor(self):
        # Query's tree API takes ref, not frame_index/start_ns/end_ns. Its
        # root+children reply must bind a real event to the separately read frame.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            c, f, e, req, reply = self.fixture(root)
            c["structural_signature"] = "Work"
            req["params"]["arguments"]["params"] = {"ref": "zone:work", "limit": 100}
            reply["result"]["structuredContent"]["data"] = {"root": {
                "ref": "zone:work", "name": "Work", "thread_ref": "thread:main",
                "start_ns": "110", "end_ns": "190", "complete": True,
                "timing_valid": True, "parent_ref": None}, "children": []}
            reply["result"]["structuredContent"]["page"] = {"next_cursor": None, "partial": False}
            e["evidence"][0]["request"] = store(root, "request.json", req)
            e["evidence"][0]["response"] = store(root, "response.json", reply)
            anchor_request = {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {
                "name": "tracy_inspect", "arguments": {"trace_id": "trace-1", "method": "frame.get",
                    "params": {"frame_set_ref": "Player.Frame", "index": 42}}}}
            anchor_reply = {"jsonrpc": "2.0", "id": 2, "result": {"structuredContent": {
                "ok": True, "trace": {"fingerprint": "a" * 64}, "data": {
                    "frame_set_ref": "Player.Frame", "index": "42", "begin_ns": "100", "end_ns": "200"}}}}
            e["evidence"].append({"evidence_id": "F42", "request": store(root, "frame-request.json", anchor_request),
                "response": store(root, "frame-response.json", anchor_reply)})
            f["investigation_receipts"][0].update(entity_json_pointers=["/data/root"],
                frame_anchor_evidence_id="F42", frame_anchor_json_pointer="/data")
            self.assertEqual([], load().validate_receipts(c, f, e, root))

            # A same-name event in the neighbouring frame must not close this one.
            reply["result"]["structuredContent"]["data"]["root"].update(start_ns="200", end_ns="250")
            e["evidence"][0]["response"] = store(root, "response.json", reply)
            self.assertTrue(load().validate_receipts(c, f, e, root))
            reply["result"]["structuredContent"]["data"]["root"].update(start_ns="110", end_ns="190")
            e["evidence"][0]["response"] = store(root, "response.json", reply)
            req["params"]["arguments"]["params"]["ref"] = "zone:unrelated"
            e["evidence"][0]["request"] = store(root, "request.json", req)
            self.assertTrue(load().validate_receipts(c, f, e, root))

    def test_three_actual_scoped_capability_failures_can_be_evidence_gap_not_success(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            c, f, e, req, reply = self.fixture(root)
            f["conclusion_status"] = "Unresolved"
            f["investigation_receipts"] = []
            e["evidence"] = []
            for i in range(3):
                req["id"] = reply["id"] = i + 1
                req["params"]["arguments"]["params"]["signature_id"] = c["signature_id"]
                reply["result"]["structuredContent"] = {"ok": False, "trace": {"fingerprint": "a" * 64}, "error": {"code": "capability_unavailable", "message": "not captured"}}
                e["evidence"].append({"evidence_id": str(i), "request": store(root, f"q{i}.json", req), "response": store(root, f"r{i}.json", reply)})
                f["investigation_receipts"].append({"evidence_id": str(i), "purpose": "zone_tree", "manifestation": "frame_cost", "outcome": "unavailable"})
            self.assertEqual([], load().validate_receipts(c, f, e, root))
            f["conclusion_status"] = "Supported"
            self.assertTrue(load().validate_receipts(c, f, e, root))
            f["conclusion_status"] = "Unresolved"
            f["investigation_receipts"].pop()
            self.assertTrue(load().validate_receipts(c, f, e, root))

    def test_unrelated_parent_nodes_cannot_prove_path(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            c, f, e, req, reply = self.fixture(root)
            nodes = reply["result"]["structuredContent"]["data"]["nodes"]
            nodes[0].pop("path")
            nodes[0]["parent_ref"] = "zone:wrong-parent"
            nodes.append({"ref": "zone:unrelated-parent", "name": "Parent"})
            f["investigation_receipts"][0]["parent_chain_json_pointers"] = ["/data/nodes/1"]
            e["evidence"][0]["response"] = store(root, "response.json", reply)
            self.assertTrue(load().validate_receipts(c, f, e, root))


if __name__ == "__main__":
    unittest.main()
