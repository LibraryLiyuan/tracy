"""Bounded, persistent MCP acceptance support; never parses raw Trace data."""

import ctypes
import argparse
import hashlib
import json
import os
import queue
import re
import subprocess
import threading
import time
from pathlib import Path


class ProcessMemoryCounters(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong),
        ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t)]


def process_memory(pid):
    if os.name != "nt":
        raise RuntimeError("Windows process memory measurement is required")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
    handle = kernel.OpenProcess(0x0400 | 0x0010, False, pid)
    if not handle:
        raise RuntimeError("process memory handle unavailable")
    try:
        counters = ProcessMemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            raise RuntimeError("process memory sample unavailable")
        return {"working_set_bytes": int(counters.WorkingSetSize),
            "peak_working_set_bytes": int(counters.PeakWorkingSetSize),
            "private_bytes": int(counters.PrivateUsage)}
    finally:
        kernel.CloseHandle(handle)


def stable_json(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def write_json(path, value):
    temporary = path.with_suffix(path.suffix+".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2)+"\n", encoding="utf-8")
    os.replace(temporary, path)


class McpClient:
    """One child, one in-flight call, bounded stdout queue and memory sampler."""
    def __init__(self, command, output, maximum_bytes=16*1024**3, max_line_bytes=32*1024**2):
        if not 0 < maximum_bytes <= 32*1024**3:
            raise ValueError("invalid MCP process budget")
        self.output = Path(output)
        self.output.mkdir(parents=True, exist_ok=True)
        self.transcript = self.output/"mcp-transcript"
        self.transcript.mkdir(exist_ok=True)
        self.stderr_file = (self.output/"query.stderr.log").open("xb")
        self.maximum_bytes = maximum_bytes
        self.max_line_bytes = max_line_bytes
        self._stop = threading.Event()
        self._lines = queue.Queue(maxsize=2)
        self._lock = threading.Lock()
        self._request_id = 0
        self.guard_error = ""
        self._metrics = {"samples": 0, "peak_working_set_bytes": 0, "peak_private_bytes": 0,
            "max_response_bytes": 0, "max_status_latency_seconds": 0.0}
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.stderr_file, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        self._reader = threading.Thread(target=self._read_stdout, daemon=True)
        self._sampler = threading.Thread(target=self._sample_memory, daemon=True)
        self._reader.start()
        self._sampler.start()

    def _put_line(self, value):
        while not self._stop.is_set():
            try:
                self._lines.put(value, timeout=.1)
                return
            except queue.Full:
                pass

    def _read_stdout(self):
        try:
            while not self._stop.is_set():
                line = self.process.stdout.readline(self.max_line_bytes+1)
                if len(line) > self.max_line_bytes:
                    raise RuntimeError("mcp_response_budget")
                self._put_line(line)
                if not line:
                    return
        except Exception as error:
            self._put_line(error)

    def sample_memory(self):
        try:
            sample = process_memory(self.process.pid)
            with self._lock:
                self._metrics["samples"] += 1
                self._metrics["peak_working_set_bytes"] = max(self._metrics["peak_working_set_bytes"], sample["peak_working_set_bytes"])
                self._metrics["peak_private_bytes"] = max(self._metrics["peak_private_bytes"], sample["private_bytes"])
                reasons = []
                if self._metrics["peak_working_set_bytes"] >= self.maximum_bytes:
                    reasons.append("working_set_limit")
                if self._metrics["peak_private_bytes"] >= self.maximum_bytes:
                    reasons.append("private_bytes_limit")
                if reasons:
                    self.guard_error = ",".join(reasons)
        except Exception:
            if self.process.poll() is None and not self._stop.is_set():
                self.guard_error = "process_memory_unavailable"

    def _sample_memory(self):
        while not self._stop.is_set() and self.process.poll() is None:
            self.sample_memory()
            self._stop.wait(.1)

    def metrics(self):
        with self._lock:
            return {**self._metrics, "query_pid": self.process.pid, "memory_guard_bytes": self.maximum_bytes,
                "memory_measurement": "OS_peak_working_set_and_100ms_plus_response_sampled_private_commit", "guard_error": self.guard_error}

    def check_guard(self):
        if self.guard_error:
            raise RuntimeError("MCP resource limit: "+self.guard_error)

    def _send(self, request):
        self.process.stdin.write(stable_json(request).encode("utf-8")+b"\n")
        self.process.stdin.flush()

    def notify(self, method, params):
        self._send({"jsonrpc": "2.0", "method": method, "params": params})

    def call(self, method, params, label, timeout=180):
        self.check_guard()
        self._request_id += 1
        request_id = self._request_id
        label = re.sub(r"[^a-zA-Z0-9_.-]", "_", label)[:100]
        request = {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}
        prefix = self.transcript/f"{request_id:05d}-{label}"
        write_json(prefix.with_suffix(".request.json"), request)
        started = time.monotonic()
        self._send(request)
        while True:
            self.check_guard()
            remaining = started+timeout-time.monotonic()
            if remaining <= 0:
                raise TimeoutError("mcp_call_timeout:"+label)
            try:
                line = self._lines.get(timeout=min(.1, remaining))
            except queue.Empty:
                continue
            if isinstance(line, Exception):
                raise line
            if not line:
                raise RuntimeError("mcp_stdout_closed:"+label)
            with self._lock:
                self._metrics["max_response_bytes"] = max(self._metrics["max_response_bytes"], len(line))
            response = json.loads(line)
            if not isinstance(response, dict):
                raise RuntimeError("mcp_non_object_message")
            if "id" not in response:
                if not isinstance(response.get("method"), str):
                    raise RuntimeError("mcp_unexpected_message")
                with (self.output/"mcp-notifications.ndjson").open("ab") as notifications:
                    notifications.write(line)
                continue
            break
        with self._lock:
            if "status" in label:
                self._metrics["max_status_latency_seconds"] = max(self._metrics["max_status_latency_seconds"], time.monotonic()-started)
        prefix.with_suffix(".response.json").write_bytes(line)
        self.sample_memory()
        self.check_guard()
        if response.get("id") != request_id:
            raise RuntimeError("mcp_response_id_mismatch")
        if "error" in response:
            raise RuntimeError("mcp_protocol_error:"+stable_json(response["error"]))
        return response

    def tool(self, name, arguments, label, timeout=180):
        response = self.call("tools/call", {"name": name, "arguments": arguments}, label, timeout)
        result = response.get("result", {})
        structured = result.get("structuredContent")
        if not isinstance(structured, dict) or result.get("isError") or structured.get("ok") is not True:
            raise RuntimeError("mcp_tool_error:"+stable_json(result))
        if structured.get("schema_version") != "1.35.0":
            raise RuntimeError("mcp_query_schema_mismatch")
        return structured

    def close(self, grace_seconds=10):
        if self.process.poll() is None:
            try:
                self.process.stdin.close()
                self.process.wait(timeout=grace_seconds)
            except Exception:
                self.process.kill()
                self.process.wait(timeout=10)
        self._stop.set()
        self._reader.join(timeout=2)
        self._sampler.join(timeout=2)
        self.process.stdout.close()
        self.stderr_file.close()


def stream_pages(client, operation, scan_id, path, selected_path=None, limit=500):
    """Export all pages and return bounded summary metadata."""
    count, cursor, total = 0, "0", None
    digest = hashlib.sha256()
    selected = 0
    samples = []
    previous_key = None
    selected_file = selected_path.open("xb") if selected_path else None
    try:
        with path.open("xb") as output:
            while True:
                response = client.tool("tracy_scan", {"operation": operation, "scan_id": scan_id,
                    "limit": limit, "cursor": cursor}, f"{operation}-page-{count}")
                data = response["data"]
                page = data["page"]
                if int(page["cursor"]) != count:
                    raise RuntimeError("pagination ordinal mismatch")
                current_total = int(page["total"])
                if total is not None and total != current_total:
                    raise RuntimeError("pagination total changed")
                total = current_total
                values = data["items"]
                if not values and not page["done"]:
                    raise RuntimeError("empty pagination did not advance")
                for row in values:
                    if operation == "signatures":
                        key = (row["domain"], row["signature_id"], row["frame_scope"])
                        if previous_key is not None and previous_key >= key:
                            raise RuntimeError("signature ordering or uniqueness failure")
                        previous_key = key
                    payload = stable_json(row).encode("utf-8") + b"\n"
                    output.write(payload)
                    digest.update(payload)
                    if count in (0, total//2, total-1):
                        samples.append({"ordinal": count, "row": row})
                    if row.get("selected") is True:
                        selected += 1
                        if selected_file:
                            selected_file.write(payload)
                    count += 1
                if page["done"] and page["next_cursor"] is None:
                    break
                next_cursor = page["next_cursor"]
                if not isinstance(next_cursor, str) or next_cursor == cursor or int(next_cursor) != count:
                    raise RuntimeError("pagination did not advance exactly")
                cursor = next_cursor
        if count != total:
            raise RuntimeError("pagination omitted records")
    finally:
        if selected_file:
            selected_file.close()
    return {"count": count, "selected": selected, "sha256": digest.hexdigest(), "samples": samples}


def read_small_json(path, maximum=16*1024*1024):
    if path.stat().st_size > maximum:
        raise RuntimeError("acceptance_control_document_budget")
    return json.loads(path.read_text(encoding="utf-8-sig"))


def file_sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def wait_trace(client, trace_id, output, timeout):
    deadline = time.monotonic()+timeout
    last_print = 0
    while True:
        response = client.tool("tracy_trace_status", {"trace_id": trace_id}, "trace-status")
        state = response["data"]["status"]["state"].lower()
        write_json(output/"trace-status.json", response)
        write_json(output/"process-metrics.json", client.metrics())
        if time.monotonic()-last_print >= 20 or state == "ready":
            print("TRACE "+stable_json({"state": state, **client.metrics()}), flush=True)
            last_print = time.monotonic()
        if state == "ready":
            return response
        if state in {"failed", "cancelled", "closed"}:
            raise RuntimeError("trace_open_failed:"+state)
        if time.monotonic() >= deadline:
            raise TimeoutError("trace_open_timeout")
        time.sleep(1)


def wait_scan(client, scan_id, output, timeout):
    deadline = time.monotonic()+timeout
    previous, last_print = "", 0
    while True:
        response = client.tool("tracy_scan", {"operation": "status", "scan_id": scan_id}, "scan-status")
        status = response["data"]
        state = status["state"].lower()
        write_json(output/"scan-status.json", response)
        write_json(output/"process-metrics.json", client.metrics())
        marker = state+stable_json(status.get("progress", {}))
        if marker != previous or time.monotonic()-last_print >= 20:
            print("SCAN "+stable_json({"status": status, **client.metrics()}), flush=True)
            previous, last_print = marker, time.monotonic()
        if status.get("completed") is True and state == "complete":
            return response
        if state in {"failed", "invalid", "closed", "cancelled_resumable"}:
            raise RuntimeError("scan_did_not_complete:"+stable_json(status))
        if time.monotonic() >= deadline:
            cancelled = client.tool("tracy_scan", {"operation": "cancel", "scan_id": scan_id}, "scan-cancel-timeout")
            write_json(output/"scan-cancelled.json", cancelled)
            raise TimeoutError("scan_wall_clock_limit")
        time.sleep(2)


def verify_samples(client, scan_id, exported, output):
    checked = []
    for operation in ("signatures", "candidates"):
        for sample in exported[operation]["samples"]:
            ordinal, row = sample["ordinal"], sample["row"]
            response = client.tool("tracy_scan", {"operation": operation, "scan_id": scan_id,
                "limit": 1, "cursor": str(ordinal)}, f"{operation}-sample-{ordinal}")
            if response["data"]["items"] != [row]:
                raise RuntimeError("sample_page_changed:"+operation)
            if operation == "candidates":
                candidate_id = row["candidate_id"]
                exact = client.tool("tracy_scan", {"operation": "candidate_get", "scan_id": scan_id,
                    "candidate_id": candidate_id}, f"candidate-get-{ordinal}")
                frames = client.tool("tracy_scan", {"operation": "representative_frames", "scan_id": scan_id,
                    "candidate_id": candidate_id}, f"candidate-frames-{ordinal}")
                if exact["data"] != row or frames["data"]["frames"] != row["representative_frame_details"]:
                    raise RuntimeError("sample_candidate_or_representatives_changed")
            checked.append({"operation": operation, "ordinal": ordinal})
    write_json(output/"samples-verified.json", {"scan_id": scan_id, "samples": checked, "verified": True})


def command_loop(client, output, trace_id, scan_id, idle_seconds):
    commands = output/"commands"
    commands.mkdir(exist_ok=True)
    write_json(output/"session-ready.json", {"trace_id": trace_id, "scan_id": scan_id,
        "query_pid": client.process.pid, "commands": str(commands)})
    deadline = time.monotonic()+idle_seconds
    while time.monotonic() < deadline:
        client.check_guard()
        for path in sorted(commands.glob("*.json")):
            destination = output/(path.stem+".results.json")
            if destination.exists():
                continue
            command = read_small_json(path, 4*1024*1024)
            if command.get("stop") is True:
                write_json(destination, {"stopped": True})
                return
            requests = command.get("requests", [])
            if not isinstance(requests, list) or len(requests) > 100:
                raise RuntimeError("command_batch_budget")
            results = []
            # Commands are explicit acceptance requests written by the caller,
            # not instructions or findings extracted from Trace payloads.
            for ordinal, entry in enumerate(requests):
                name = entry.get("name", "tracy_inspect")
                arguments = entry.get("arguments", {})
                if name == "tracy_inspect":
                    arguments = {"trace_id": trace_id, "method": entry["method"], "params": entry.get("params", {})}
                try:
                    response = client.tool(name, arguments, entry["label"])
                    result = {"label": entry["label"], "response": response}
                    ok = True
                except Exception as error:
                    result = {"label": entry["label"], "error": str(error)}
                    ok = False
                    client.check_guard()
                result_name = f"{path.stem}-{ordinal:03d}.result.json"
                write_json(output/result_name, result)
                results.append({"label": entry["label"], "result_file": result_name, "ok": ok})
            write_json(destination, {"requests": results})
            print("COMMAND_COMPLETE "+path.name, flush=True)
            deadline = time.monotonic()+idle_seconds
        write_json(output/"process-metrics.json", client.metrics())
        time.sleep(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--query", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--analysis-root", type=Path)
    parser.add_argument("--cache-root", type=Path)
    parser.add_argument("--allow-root", type=Path)
    parser.add_argument("--mode", choices=("contract", "scan", "export", "reopen"), default="contract")
    parser.add_argument("--trace", type=Path)
    parser.add_argument("--expected-trace-sha256")
    parser.add_argument("--expected-profile-identity")
    parser.add_argument("--scan-id", default="")
    parser.add_argument("--expected-export", type=Path)
    parser.add_argument("--scan-timeout-seconds", type=int, default=7200)
    parser.add_argument("--idle-seconds", type=int, default=0)
    parser.add_argument("--process-memory-limit-gib", type=int, choices=range(1,33), default=16)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        raise RuntimeError("choose_a_fresh_acceptance_output_directory")
    output.mkdir(parents=True)
    query, profile_path = args.query.resolve(), args.profile.resolve()
    profile = read_small_json(profile_path, 4*1024*1024)
    if profile.get("user_focus") != []:
        raise RuntimeError("acceptance_requires_unmodified_empty_user_focus")
    open_source = args.mode == "scan" or (args.mode == "export" and args.trace is not None)
    if open_source and (not args.trace or not args.expected_trace_sha256):
        raise RuntimeError("scan_requires_explicit_trace_and_expected_sha256")
    if args.mode == "export" and not args.scan_id:
        raise RuntimeError("export_requires_completed_scan_id")
    if args.mode == "reopen" and (not args.scan_id or not args.expected_export):
        raise RuntimeError("reopen_requires_scan_id_and_previous_export")
    trace = args.trace.resolve() if args.trace else None
    identity = {"mode": args.mode, "query": str(query), "query_sha256": file_sha256(query),
        "profile": str(profile_path), "profile_file_sha256": file_sha256(profile_path),
        "trace": str(trace) if trace else None, "trace_sha256": None, "indexed": False,
        "source_open_requested": open_source, "source_scan_requested": args.mode == "scan",
        "process_memory_limit_bytes": args.process_memory_limit_gib*1024**3}
    if open_source:
        identity["trace_sha256"] = file_sha256(trace)
        if identity["trace_sha256"].lower() != args.expected_trace_sha256.lower():
            raise RuntimeError("trace_identity_mismatch")
    write_json(output/"identity.json", identity)
    analysis_root = (args.analysis_root or output/"analysis").resolve()
    cache_root = (args.cache_root or analysis_root/"cache").resolve()
    allow_root = (args.allow_root or (trace.parent if trace else output)).resolve()
    if not cache_root.is_relative_to(analysis_root):
        raise RuntimeError("analysis_cache_root_must_be_inside_analysis_root")
    analysis_root.mkdir(parents=True, exist_ok=True)
    cache_root.mkdir(parents=True, exist_ok=True)
    identity.update({"analysis_root": str(analysis_root), "cache_root": str(cache_root), "allow_root": str(allow_root)})
    write_json(output/"identity.json", identity)
    command = [str(query), "--mcp", "--no-indexed", "--allow-root", str(allow_root),
        "--analysis-root", str(analysis_root), "--analysis-cache-root", str(cache_root)]
    started = time.monotonic()
    client, trace_id, scan_id, failure = None, "", args.scan_id, None
    completed = False
    try:
        client = McpClient(command, output, maximum_bytes=args.process_memory_limit_gib*1024**3)
        initialized = client.call("initialize", {"protocolVersion": "2025-11-25",
            "clientInfo": {"name": "query-cache-acceptance", "version": "1"}, "capabilities": {}}, "initialize")
        if initialized["result"]["protocolVersion"] != "2025-11-25":
            raise RuntimeError("mcp_version_mismatch")
        client.notify("notifications/initialized", {})
        tools = client.call("tools/list", {}, "tools-list")
        names = {entry["name"] for entry in tools["result"]["tools"]}
        if not {"tracy_scan", "tracy_trace_open", "tracy_trace_status", "tracy_inspect"}.issubset(names):
            raise RuntimeError("required_mcp_tools_missing")
        write_json(output/"tools.json", tools)
        validation = client.tool("tracy_scan", {"operation": "profile_validate", "profile": profile}, "profile-validate")
        write_json(output/"profile-validation.json", validation)
        if args.expected_profile_identity and validation["data"]["profile_identity"] != args.expected_profile_identity:
            raise RuntimeError("profile_identity_changed")
        if args.mode == "contract":
            completed = True
            print("CONTRACT_COMPLETE "+stable_json(client.metrics()), flush=True)
            return 0
        if open_source:
            opened = client.tool("tracy_trace_open", {"path": str(trace)}, "trace-open")
            trace_id = opened["data"]["trace_id"]
            write_json(output/"trace-ready.json", wait_trace(client, trace_id, output, 900))
            write_json(output/"overview.json", client.tool("tracy_overview", {"trace_id": trace_id}, "overview"))
        if args.mode == "scan":
            scan_start = client.tool("tracy_scan", {"operation": "start", "trace_id": trace_id, "profile": profile}, "scan-start")
            scan_id = scan_start["data"]["scan_id"]
            write_json(output/"scan-start.json", scan_start)
            write_json(output/"scan-complete.json", wait_scan(client, scan_id, output, args.scan_timeout_seconds))
        else:
            status = client.tool("tracy_scan", {"operation": "status", "scan_id": scan_id},
                "cold-status" if args.mode == "reopen" else "cache-status")
            if status["data"].get("completed") is not True:
                raise RuntimeError("cold_cache_is_not_complete")
            write_json(output/"scan-complete.json", status)
        summary = client.tool("tracy_scan", {"operation": "summary", "scan_id": scan_id}, "scan-summary")
        quality = client.tool("tracy_scan", {"operation": "quality", "scan_id": scan_id}, "scan-quality")
        write_json(output/"scan-summary.json", summary)
        write_json(output/"scan-quality.json", quality)
        exported = {"scan_id": scan_id, "summary": summary["data"], "quality": quality["data"]}
        for operation in ("signatures", "candidates"):
            exported[operation] = stream_pages(client, operation, scan_id, output/(operation+".ndjson"),
                output/"selected-candidates.ndjson" if operation == "candidates" else None)
            print("PAGED "+stable_json({"operation": operation, "count": exported[operation]["count"], **client.metrics()}), flush=True)
        backlog = summary["data"]["backlog"]
        if exported["candidates"]["count"] != int(backlog["total"]) or exported["candidates"]["selected"] != int(backlog["selected"]):
            raise RuntimeError("candidate_backlog_mismatch")
        verify_samples(client, scan_id, exported, output)
        if args.expected_export:
            previous = read_small_json(args.expected_export)
            for key in ("scan_id", "summary", "quality"):
                if previous[key] != exported[key]:
                    raise RuntimeError("cold_export_changed:"+key)
            for operation in ("signatures", "candidates"):
                if previous[operation] != exported[operation]:
                    raise RuntimeError("cold_export_changed:"+operation)
        write_json(output/"export-summary.json", exported)
        completed = True
        print("CACHE_COMPLETE "+stable_json({"scan_id": scan_id, "signatures": exported["signatures"]["count"],
            "candidates": exported["candidates"]["count"], "selected": exported["candidates"]["selected"], **client.metrics()}), flush=True)
        if args.idle_seconds > 0:
            command_loop(client, output, trace_id, scan_id, args.idle_seconds)
        if trace_id:
            client.tool("tracy_trace_close", {"trace_id": trace_id}, "trace-close")
        return 0
    except Exception as error:
        failure = {"type": type(error).__name__, "message": str(error)}
        raise
    finally:
        if client:
            try:
                client.close()
            finally:
                write_json(output/"run-outcome.json", {"mode": args.mode, "completed": completed,
                    "trace_id": trace_id, "scan_id": scan_id, "failure": failure,
                    "elapsed_seconds": time.monotonic()-started, "query_exit_code": client.process.poll(), **client.metrics()})


if __name__ == "__main__":
    raise SystemExit(main())
