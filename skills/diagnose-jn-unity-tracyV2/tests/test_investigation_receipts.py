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

    def test_nested_has_more_without_cursor_is_not_complete_evidence(self):
        # gpu.pass.search currently exposes data.page.has_more without an
        # envelope cursor. Accepting this as exhausted could falsely prove
        # target absence or uniqueness from the first 1,000 of 1,077 rows.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            c, f, e, _, reply = self.fixture(root)
            content = reply["result"]["structuredContent"]
            content.update(partial=False, page={"next_cursor": None, "partial": False})
            content["data"]["page"] = {"has_more": True, "returned": 1000, "total": 1077}
            e["evidence"][0]["response"] = store(root, "response.json", reply)
            errors = load().validate_receipts(c, f, e, root)
            self.assertTrue(any("receipt_query_incomplete" in error for error in errors), errors)

    def test_ignored_signature_fields_without_entity_anchor_cannot_close_gap(self):
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
            self.assertTrue(load().validate_receipts(c, f, e, root))
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

    def test_parent_chain_can_use_actual_separate_get_receipts(self):
        # Production zone.tree returns root + children, never its ancestors.
        # Ancestor get calls must be hash/identity/ref verified independently.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            c, f, e, req, reply = self.fixture(root)
            node = reply["result"]["structuredContent"]["data"]["nodes"][0]
            node.pop("path")
            node["parent_ref"] = "zone:parent"
            e["evidence"][0]["response"] = store(root, "response.json", reply)
            parent_req = json.loads(json.dumps(req))
            parent_req["id"] = 2
            parent_req["params"]["arguments"].update(method="zone.cpu.get", params={"ref": "zone:parent"})
            parent_reply = {"jsonrpc": "2.0", "id": 2, "result": {"structuredContent": {
                "ok": True, "trace": {"fingerprint": "a" * 64}, "data": {
                    "ref": "zone:parent", "name": "Parent", "parent_ref": None, "thread_ref": "thread:main"}}}}
            e["evidence"].append({"evidence_id": "P1", "request": store(root, "parent-q.json", parent_req),
                "response": store(root, "parent-r.json", parent_reply)})
            f["investigation_receipts"][0]["parent_chain_evidence"] = [{"evidence_id": "P1", "entity_json_pointer": "/data"}]
            self.assertEqual([], load().validate_receipts(c, f, e, root))
            parent_reply["result"]["structuredContent"]["data"]["ref"] = "zone:unrelated"
            e["evidence"][-1]["response"] = store(root, "parent-r.json", parent_reply)
            self.assertTrue(load().validate_receipts(c, f, e, root))

    def test_frame_wall_signature_uses_real_root_even_when_legacy_flag_is_false(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            c, f, e, _, reply = self.fixture(root)
            c.update(frame_root=False, signature_id="frame-wall:Player.Frame", structural_signature="Player.Frame")
            reply["result"]["structuredContent"]["data"]["nodes"][0].update(
                name="Main.PlayerLoop", path="Main.PlayerLoop", parent_ref=None, complete=True)
            e["evidence"][0]["response"] = store(root, "response.json", reply)
            self.assertEqual([], load().validate_receipts(c, f, e, root))
            c["signature_id"] = "frame-wall:Other.Frame"
            self.assertTrue(load().validate_receipts(c, f, e, root))

    def gpu_fixture(self, root):
        c, f, e, req, reply = self.fixture(root)
        c.update(domain="gpu", structural_signature="GPU.Frame.Direct > Work",
            thread_or_queue="gpu-context:0", observation_unit="l0_segment", representative_frames=[],
            representative_intervals=[{"begin_ns": "100", "end_ns": "200", "event_refs": ["gpu-zone:l0"]}])
        req["params"]["arguments"].update(method="zone.gpu.tree", params={"ref": "gpu-zone:work"})
        reply["result"]["structuredContent"]["data"] = {"root": {
            "ref": "gpu-zone:work", "name": "Work", "thread_ref": "thread:render",
            "context_ref": "gpu-context:0", "parent_ref": "gpu-zone:l0", "complete": True,
            "gpu_start_ns": "110", "gpu_end_ns": "190"}, "children": []}
        parent_req = json.loads(json.dumps(req)); parent_req["id"] = 2
        parent_req["params"]["arguments"].update(method="zone.gpu.get", params={"ref": "gpu-zone:l0"})
        parent_reply = {"jsonrpc": "2.0", "id": 2, "result": {"structuredContent": {
            "ok": True, "trace": {"fingerprint": "a" * 64}, "data": {
                "ref": "gpu-zone:l0", "name": "GPU.Frame.Direct", "parent_ref": None,
                "thread_ref": "thread:render", "context_ref": "gpu-context:0", "complete": True,
                "gpu_start_ns": "100", "gpu_end_ns": "200"}}}}
        e["evidence"].append({"evidence_id": "GPU0"})
        f["investigation_receipts"][0].update(entity_json_pointers=["/data/root"],
            parent_chain_evidence=[{"evidence_id": "GPU0", "entity_json_pointer": "/data"}],
            interval_anchor_evidence_id="GPU0", interval_anchor_json_pointer="/data")
        return c, f, e, req, reply, parent_req, parent_reply

    def save_gpu_fixture(self, root, e, req, reply, parent_req, parent_reply):
        e["evidence"][0].update(request=store(root, "gpu-q.json", req), response=store(root, "gpu-r.json", reply))
        e["evidence"][1].update(request=store(root, "anchor-q.json", parent_req), response=store(root, "anchor-r.json", parent_reply))

    def test_gpu_ref_tree_uses_context_and_proven_l0_anchor(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            c, f, e, req, reply, pq, pr = self.gpu_fixture(root)
            self.save_gpu_fixture(root, e, req, reply, pq, pr)
            self.assertEqual([], load().validate_receipts(c, f, e, root))
            # CPU producer thread identity must not be mistaken for a GPU queue.
            reply["result"]["structuredContent"]["data"]["root"]["thread_ref"] = "thread:other-producer"
            self.save_gpu_fixture(root, e, req, reply, pq, pr)
            self.assertEqual([], load().validate_receipts(c, f, e, root))

    def test_gpu_l0_anchor_rejects_wrong_queue_scope_or_unrelated_event(self):
        for mutation in ["wrong_queue", "parent_queue", "wrong_ref", "other_interval", "unrelated_parent",
                         "missing_anchor", "missing_time", "outside_interval", "partial_anchor", "wrong_tree_ref"]:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                c, f, e, req, reply, pq, pr = self.gpu_fixture(root)
                node = reply["result"]["structuredContent"]["data"]["root"]
                parent = pr["result"]["structuredContent"]["data"]
                if mutation == "wrong_queue": node["context_ref"] = "gpu-context:1"
                elif mutation == "parent_queue": parent["context_ref"] = "gpu-context:1"
                elif mutation == "wrong_ref": c["representative_intervals"][0]["event_refs"] = ["gpu-zone:other"]
                elif mutation == "other_interval": parent["gpu_start_ns"] = "99"
                elif mutation == "unrelated_parent": node["parent_ref"] = "gpu-zone:other"
                elif mutation == "missing_anchor": f["investigation_receipts"][0].pop("interval_anchor_evidence_id")
                elif mutation == "missing_time": node.pop("gpu_end_ns")
                elif mutation == "outside_interval": node["gpu_end_ns"] = "201"
                elif mutation == "partial_anchor": pr["result"]["structuredContent"]["partial"] = True
                elif mutation == "wrong_tree_ref": req["params"]["arguments"]["params"]["ref"] = "gpu-zone:other"
                self.save_gpu_fixture(root, e, req, reply, pq, pr)
                self.assertTrue(load().validate_receipts(c, f, e, root))


if __name__ == "__main__":
    unittest.main()
