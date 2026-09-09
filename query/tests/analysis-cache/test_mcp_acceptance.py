"""Acceptance transport regressions. No real Query or Trace is opened."""
import ctypes
import os
import json
import hashlib
from pathlib import Path
import tempfile
import tracemalloc
import sys
import time
import unittest
from unittest.mock import patch
import mcp_acceptance as runner


@unittest.skipUnless(os.name == "nt", "Windows memory accounting")
class MemoryTests(unittest.TestCase):
    def test_sample_is_real_and_missing_process_is_not_zero(self):
        value = runner.process_memory(os.getpid())
        self.assertGreater(value["working_set_bytes"], 0)
        self.assertGreater(value["private_bytes"], 0)
        self.assertGreaterEqual(value["peak_working_set_bytes"], value["working_set_bytes"])
        with self.assertRaises(RuntimeError):
            runner.process_memory(0xffffffff)

    def test_private_commit_is_not_reported_as_peak_working_set(self):
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong, ctypes.c_ulong]
        kernel.VirtualAlloc.restype = ctypes.c_void_p
        kernel.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong]
        before = runner.process_memory(os.getpid())
        allocation = kernel.VirtualAlloc(None, 64*1024*1024, 0x3000, 0x04)
        self.assertTrue(allocation)
        try:
            after = runner.process_memory(os.getpid())
            # Committed but untouched memory changes private commit without
            # adding 64 MiB of resident pages to this actual test process.
            self.assertGreaterEqual(after["private_bytes"]-before["private_bytes"], 64*1024*1024)
            self.assertLess(after["working_set_bytes"]-before["working_set_bytes"], 16*1024*1024)
        finally:
            self.assertTrue(kernel.VirtualFree(allocation, 0, 0x8000))


class Pages:
    def __init__(self, count=5000, corrupt_cursor=False):
        self.count = count
        self.corrupt_cursor = corrupt_cursor

    def tool(self, name, arguments, label):
        if name != "tracy_scan" or arguments["operation"] != "candidates":
            raise AssertionError("unexpected transport request")
        # Match the public MCP validation, not the more permissive internal API.
        if "cursor" in arguments and not arguments["cursor"]:
            raise RuntimeError("cursor must be a non-empty string of at most 256 bytes")
        offset = int(arguments.get("cursor", "0"))
        end = min(self.count, offset+arguments["limit"])
        rows = [{"candidate_id": f"candidate:{i}", "domain": "cpu", "signature_id": f"sig-{i:08d}",
            "frame_scope": "fixture", "selected": i % 2 == 0,
            "payload": str(i)+"x"*4096} for i in range(offset, end)]
        return {"ok": True, "schema_version": "1.35.0", "data": {"items": rows, "page": {
            "cursor": str(offset), "total": str(self.count), "done": end == self.count,
            "next_cursor": None if end == self.count else str(end+(1 if self.corrupt_cursor else 0))}}}


class PaginationTests(unittest.TestCase):
    def test_export_keeps_one_page_not_the_whole_candidate_table(self):
        with tempfile.TemporaryDirectory(prefix="cache-mcp-pages-") as directory:
            path = Path(directory)/"candidates.ndjson"
            selected = Path(directory)/"selected.ndjson"
            tracemalloc.start()
            try:
                try:
                    result = runner.stream_pages(Pages(), "candidates", "scan-fixture", path, selected, limit=100)
                except RuntimeError as error:
                    self.fail("Public MCP rejected pagination: "+str(error))
                peak = tracemalloc.get_traced_memory()[1]
            finally:
                tracemalloc.stop()
            self.assertLess(peak, 8*1024*1024, "client retained the complete 20 MiB table instead of bounded pages")
            self.assertEqual(result["count"], 5000)
            self.assertEqual(result["selected"], 2500)
            digest = hashlib.sha256()
            with path.open("rb") as rows:
                for count, line in enumerate(rows, 1):
                    digest.update(line)
                    self.assertEqual(json.loads(line)["candidate_id"], f"candidate:{count-1}")
            self.assertEqual(count, 5000)
            self.assertEqual(result["sha256"], digest.hexdigest())
            with selected.open("rb") as rows:
                self.assertEqual(sum(1 for _ in rows), 2500)
            self.assertEqual([sample["ordinal"] for sample in result["samples"]], [0,2500,4999])

    def test_skipped_cursor_does_not_publish_a_false_complete_count(self):
        with tempfile.TemporaryDirectory(prefix="cache-mcp-cursor-") as directory:
            with self.assertRaisesRegex(RuntimeError, "advance"):
                runner.stream_pages(Pages(20, True), "candidates", "scan-fixture", Path(directory)/"partial.ndjson", limit=5)


def make_client(directory, **options):
    return runner.McpClient([sys.executable, "-B", str(Path(__file__).with_name("fake_mcp_stdio.py"))], Path(directory), **options)


class CompletedCachePeer:
    """Strict external Query substitute: completed-cache reads, never a scan."""
    def __init__(self, command, output, **options):
        self.process = self
        self.pid = 123
        self.closed = False
        self.opened = False
        self.signature = {"domain": "cpu", "signature_id": "cpu:fixture", "frame_scope": "Player.Frame"}
        self.candidate = {"candidate_id": "candidate:fixture", "selected": True,
            "representative_frame_details": []}

    def call(self, method, params, label):
        if method == "initialize":
            return {"result": {"protocolVersion": "2025-11-25"}}
        if method == "tools/list":
            return {"result": {"tools": [{"name": name} for name in
                ("tracy_scan", "tracy_trace_open", "tracy_trace_status", "tracy_inspect")]}}
        raise AssertionError("Unexpected cache-recovery transport method: "+method)

    def notify(self, method, params):
        if method != "notifications/initialized":
            raise AssertionError("Unexpected notification")

    def tool(self, name, arguments, label, **options):
        if name == "tracy_trace_open":
            if self.opened:
                raise AssertionError("Recovery opened the source more than once")
            self.opened = True
            data = {"trace_id": "trace-fixture"}
        elif name == "tracy_trace_status":
            data = {"status": {"state": "ready"}}
        elif name == "tracy_overview":
            data = {}
        elif name == "tracy_trace_close":
            data = {"closed": True}
        elif name == "tracy_scan":
            operation = arguments["operation"]
            if operation == "profile_validate":
                data = {"profile_identity": "profile-fixture"}
            elif operation == "status":
                if arguments["scan_id"] != "scan-complete-fixture":
                    raise AssertionError("Recovery queried a different scan")
                data = {"completed": True, "state": "complete"}
            elif operation == "summary":
                data = {"backlog": {"total": 1, "selected": 1, "not_selected": 0}}
            elif operation == "quality":
                data = {"complete": True, "findings": []}
            elif operation in ("signatures", "candidates"):
                if arguments.get("cursor") != "0":
                    raise AssertionError("Invalid public MCP first-page cursor")
                data = {"items": [self.signature if operation == "signatures" else self.candidate],
                    "page": {"cursor": "0", "total": "1", "done": True, "next_cursor": None}}
            elif operation == "candidate_get":
                data = self.candidate
            elif operation == "representative_frames":
                data = {"frames": []}
            else:
                raise AssertionError("Completed-cache export must never start/resume a scan: "+operation)
        else:
            raise AssertionError("Unexpected cache-recovery tool: "+name)
        return {"ok": True, "schema_version": "1.35.0", "data": data}

    def metrics(self):
        return {"query_pid": self.pid, "guard_error": ""}

    def close(self):
        self.closed = True

    def poll(self):
        return 0 if self.closed else None


class RecoveryTests(unittest.TestCase):
    def test_export_reuses_complete_cache_with_optional_evidence_source_but_never_rescans(self):
        for attach_source in (False, True):
            with self.subTest(attach_source=attach_source), tempfile.TemporaryDirectory(prefix="cache-mcp-recover-") as directory:
                root = Path(directory)
                query, profile, trace, output = root/"query.fixture", root/"profile.json", root/"source.fixture", root/"output"
                query.write_bytes(b"test-only Query identity")
                profile.write_text('{"user_focus":[]}', encoding="utf-8")
                trace.write_bytes(b"test-only source identity; not a real Trace")
                arguments = ["mcp_acceptance.py", "--mode", "export", "--query", str(query),
                    "--profile", str(profile), "--output", str(output), "--scan-id", "scan-complete-fixture",
                    "--expected-profile-identity", "profile-fixture", "--process-memory-limit-gib", "32"]
                if attach_source:
                    arguments += ["--trace", str(trace), "--expected-trace-sha256", runner.file_sha256(trace)]
                with patch.object(runner, "McpClient", CompletedCachePeer), patch.object(sys, "argv", arguments):
                    try:
                        result = runner.main()
                    except SystemExit as error:
                        self.fail("Completed-cache export was rejected: "+str(error))
                self.assertEqual(result, 0)
                outcome = runner.read_small_json(output/"run-outcome.json")
                self.assertTrue(outcome["completed"])
                self.assertEqual(outcome["mode"], "export")
                self.assertEqual(outcome["trace_id"], "trace-fixture" if attach_source else "")
                exported = runner.read_small_json(output/"export-summary.json")
                self.assertEqual(exported["signatures"]["count"], 1)
                self.assertEqual(exported["candidates"]["selected"], 1)


class TransportTests(unittest.TestCase):

    def test_explicit_32_gib_budget_allows_persistent_rpc_but_not_a_larger_limit(self):
        with tempfile.TemporaryDirectory(prefix="cache-mcp-32g-") as directory:
            try:
                client = make_client(directory, maximum_bytes=32*1024**3)
            except ValueError as error:
                self.fail(f"explicit 32 GiB budget rejected before RPC: {error}")
            try:
                response = client.call("echo", {"index": 32}, "authorized-32g")
                self.assertEqual(response["result"]["echo"]["index"], 32)
            finally:
                client.close()
        with tempfile.TemporaryDirectory(prefix="cache-mcp-over32g-") as directory:
            with self.assertRaisesRegex(ValueError, "process budget"):
                make_client(directory, maximum_bytes=32*1024**3+1)

    def test_notifications_do_not_replace_replies_in_one_persistent_child(self):
        with tempfile.TemporaryDirectory(prefix="cache-mcp-peer-") as directory:
            client = make_client(directory)
            try:
                pid = client.process.pid
                for index in range(3):
                    try:
                        response = client.call("echo", {"index": index, "text": "完整引用"}, f"call-{index}")
                    except RuntimeError as error:
                        if "response_id_mismatch" not in str(error):
                            raise
                        self.fail("A valid progress notification replaced the requested MCP reply")
                    self.assertEqual(response["result"]["echo"], {"index": index, "text": "完整引用"})
                    self.assertEqual(client.process.pid, pid)
            finally:
                client.close()


class CommandTests(unittest.TestCase):
    def test_command_batch_writes_each_response_without_retaining_the_batch(self):
        class Peer:
            def check_guard(self):
                pass
            def tool(self, name, arguments, label):
                return {"ok": True, "schema_version": "1.35.0", "data": {"payload": label+"x"*(2*1024*1024)}}
            class process:
                pid = 123
        with tempfile.TemporaryDirectory(prefix="cache-mcp-commands-") as directory:
            root = Path(directory)
            commands = root/"commands"
            commands.mkdir()
            (commands/"01-batch.json").write_text(json.dumps({"requests": [
                {"name": "tracy_scan", "arguments": {"operation": "summary", "scan_id": "fixture"}, "label": str(i)}
                for i in range(10)]}), encoding="utf-8")
            (commands/"02-stop.json").write_text('{"stop":true}', encoding="utf-8")
            tracemalloc.start()
            try:
                runner.command_loop(Peer(), root, "trace-fixture", "scan-fixture", 1)
                peak = tracemalloc.get_traced_memory()[1]
            finally:
                tracemalloc.stop()
            self.assertLess(peak, 12*1024*1024, "command loop retained every response in the batch")
            manifest = json.loads((root/"01-batch.results.json").read_text(encoding="utf-8"))
            self.assertEqual(len(manifest["requests"]), 10)
            self.assertLess((root/"01-batch.results.json").stat().st_size, 4096)
            for index, item in enumerate(manifest["requests"]):
                self.assertTrue(item["ok"])
                response = json.loads((root/item["result_file"]).read_text(encoding="utf-8"))
                self.assertTrue(response["response"]["data"]["payload"].startswith(str(index)))

    def test_response_limit_and_timeout_fail_explicitly(self):
        for method, expected in (("oversize", "response_budget"), ("hang", "call_timeout")):
            with tempfile.TemporaryDirectory(prefix="cache-mcp-limit-") as directory:
                client = make_client(directory, max_line_bytes=1024)
                try:
                    with self.assertRaisesRegex((RuntimeError, TimeoutError), expected):
                        client.call(method, {}, method, timeout=.15)
                finally:
                    client.close(grace_seconds=.1)
                self.assertIsNotNone(client.process.poll())

    def test_memory_guard_does_not_read_a_zero_for_a_live_child(self):
        with tempfile.TemporaryDirectory(prefix="cache-mcp-memory-") as directory:
            client = make_client(directory, maximum_bytes=1)
            try:
                deadline = time.monotonic()+3
                while not client.guard_error and time.monotonic() < deadline:
                    time.sleep(.01)
                with self.assertRaisesRegex(RuntimeError, "resource limit"):
                    client.call("echo", {}, "guarded")
                self.assertGreater(client.metrics()["peak_private_bytes"], 1)
            finally:
                client.close()


if __name__ == "__main__":
    unittest.main()
