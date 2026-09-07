"""Line-delimited MCP client used only by the deterministic acceptance harness."""

from __future__ import annotations

from collections import deque
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time
from typing import Any, Mapping, Sequence

from .win32_metrics import (
    ProcessIdentity,
    ProcessMetricError,
    capture_process_identity,
    same_process,
    sample_process_tree,
)


class McpContractError(RuntimeError):
    pass


class McpTimeoutError(McpContractError, TimeoutError):
    """A response deadline, not evidence of semantic data corruption."""


class McpClient:
    """One audited MCP process with bounded waits and process-tree metrics."""

    def __init__(
        self,
        command: Sequence[str],
        *,
        cwd: Path,
        raw_log_path: Path,
        stderr_path: Path,
        metrics_path: Path,
        sample_interval_seconds: float = 0.1,
    ):
        if not command:
            raise ValueError("MCP command is empty")
        if sample_interval_seconds <= 0:
            raise ValueError("sample_interval_seconds must be positive")
        self.command = tuple(str(value) for value in command)
        self.cwd = Path(cwd)
        self.raw_log_path = Path(raw_log_path)
        self.stderr_path = Path(stderr_path)
        self.metrics_path = Path(metrics_path)
        self.sample_interval_seconds = sample_interval_seconds
        self.process: subprocess.Popen[str] | None = None
        self.identity: ProcessIdentity | None = None
        self.peak_private_bytes = 0
        self.peak_working_set_bytes = 0
        self._next_id = 1
        self._messages: queue.Queue[Any] = queue.Queue()
        self._pending: dict[tuple[type, Any], dict[str, Any]] = {}
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []
        self._log_lock = threading.Lock()
        self._sequence = 0

    def __enter__(self) -> "McpClient":
        for path in (self.raw_log_path, self.stderr_path, self.metrics_path):
            path.parent.mkdir(parents=True, exist_ok=True)
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == "win32" else 0
        self.process = subprocess.Popen(
            self.command,
            cwd=str(self.cwd),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            shell=False,
            creationflags=creationflags,
        )
        try:
            self.identity = capture_process_identity(self.process.pid)
        except ProcessMetricError:
            self.identity = None
        self._threads = [
            threading.Thread(target=self._read_stdout, name="n30a-mcp-stdout", daemon=True),
            threading.Thread(target=self._read_stderr, name="n30a-mcp-stderr", daemon=True),
            threading.Thread(target=self._sample_metrics, name="n30a-mcp-metrics", daemon=True),
        ]
        for thread in self._threads:
            thread.start()
        self._sample_once()
        print(
            f"[MCP][START] pid={self.process.pid} exe={self.command[0]} cwd={self.cwd}",
            flush=True,
        )
        return self

    def _append_raw(self, direction: str, message: Mapping[str, Any]) -> None:
        with self._log_lock:
            self._sequence += 1
            record = {
                "sequence": self._sequence,
                "direction": direction,
                "message": dict(message),
            }
            with self.raw_log_path.open("a", encoding="utf-8", newline="\n") as stream:
                stream.write(
                    json.dumps(record, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
                    + "\n"
                )
                stream.flush()
                os.fsync(stream.fileno())

    def _read_stdout(self) -> None:
        assert self.process is not None and self.process.stdout is not None
        try:
            for line in self.process.stdout:
                if not line.strip():
                    continue
                try:
                    message = json.loads(line)
                except json.JSONDecodeError as error:
                    self._messages.put(McpContractError(f"invalid MCP JSON: {error}"))
                    continue
                if not isinstance(message, dict):
                    self._messages.put(McpContractError("MCP response is not an object"))
                    continue
                self._append_raw("response", message)
                self._messages.put(message)
        finally:
            self._messages.put(EOFError("MCP stdout closed"))

    def _read_stderr(self) -> None:
        assert self.process is not None and self.process.stderr is not None
        with self.stderr_path.open("a", encoding="utf-8", newline="\n") as target:
            for line in self.process.stderr:
                target.write(line)
                target.flush()

    def _sample_once(self) -> None:
        if self.process is None or self.process.poll() is not None:
            return
        try:
            sample = sample_process_tree(self.process.pid)
        except ProcessMetricError as error:
            record = {
                "timestamp_monotonic_ns": str(time.monotonic_ns()),
                "metric_error": str(error),
            }
        else:
            self.peak_private_bytes = max(
                self.peak_private_bytes, sample.private_bytes, sample.peak_private_bytes
            )
            self.peak_working_set_bytes = max(
                self.peak_working_set_bytes,
                sample.working_set_bytes,
                sample.peak_working_set_bytes,
            )
            record = sample.as_dict()
        with self.metrics_path.open("a", encoding="utf-8", newline="\n") as stream:
            stream.write(json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n")
            stream.flush()

    def _sample_metrics(self) -> None:
        while not self._stop.wait(self.sample_interval_seconds):
            self._sample_once()

    def _send(self, message: Mapping[str, Any]) -> None:
        if self.process is None or self.process.stdin is None:
            raise McpContractError("MCP process is not running")
        if self.process.poll() is not None:
            raise McpContractError(f"MCP process exited with {self.process.returncode}")
        self._append_raw("request", message)
        self.process.stdin.write(
            json.dumps(message, ensure_ascii=False, separators=(",", ":")) + "\n"
        )
        self.process.stdin.flush()

    def notify(self, method: str, params: Mapping[str, Any] | None = None) -> None:
        self._send({"jsonrpc": "2.0", "method": method, "params": dict(params or {})})

    def request(
        self,
        method: str,
        params: Mapping[str, Any] | None = None,
        *,
        timeout_seconds: float,
    ) -> dict[str, Any]:
        if timeout_seconds <= 0:
            raise ValueError("timeout_seconds must be positive")
        identifier = self._next_id
        self._next_id += 1
        key = (int, identifier)
        request = {
            "jsonrpc": "2.0",
            "id": identifier,
            "method": method,
            "params": dict(params or {}),
        }
        self._send(request)
        deadline = time.monotonic() + timeout_seconds
        while True:
            pending = self._pending.pop(key, None)
            if pending is not None:
                return pending
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise McpTimeoutError(
                    f"MCP request {method!r} id={identifier} timed out after {timeout_seconds}s"
                )
            try:
                value = self._messages.get(timeout=remaining)
            except queue.Empty as error:
                raise McpTimeoutError(
                    f"MCP request {method!r} id={identifier} timed out after {timeout_seconds}s"
                ) from error
            if isinstance(value, BaseException):
                raise McpContractError(str(value)) from value
            response_id = value.get("id")
            response_key = (type(response_id), response_id)
            if response_key == key:
                return value
            if response_id is not None:
                self._pending[response_key] = value

    def call_tool(
        self,
        name: str,
        arguments: Mapping[str, Any],
        *,
        timeout_seconds: float,
    ) -> dict[str, Any]:
        response = self.request(
            "tools/call",
            {"name": name, "arguments": dict(arguments)},
            timeout_seconds=timeout_seconds,
        )
        if "error" in response:
            raise McpContractError(f"MCP JSON-RPC error for {name}: {response['error']}")
        result = response.get("result")
        if not isinstance(result, Mapping):
            raise McpContractError(f"MCP tool {name} omitted result")
        structured = result.get("structuredContent")
        if not isinstance(structured, Mapping):
            raise McpContractError(f"MCP tool {name} omitted structuredContent")
        return dict(structured)

    def close(self) -> None:
        if self.process is None:
            return
        self._stop.set()
        if self.process.stdin is not None:
            try:
                self.process.stdin.close()
            except OSError:
                pass
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            if self.identity is None or not same_process(self.identity):
                raise McpContractError(
                    "refusing to terminate MCP process because its identity changed"
                )
            self.process.kill()
            self.process.wait(timeout=5)
        for thread in self._threads:
            thread.join(timeout=1)
        self._sample_once()
        for stream in (self.process.stdout, self.process.stderr):
            if stream is not None and not stream.closed:
                stream.close()
        print(
            f"[MCP][END] pid={self.process.pid} exit={self.process.returncode} "
            f"peak_private_bytes={self.peak_private_bytes}",
            flush=True,
        )

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()
