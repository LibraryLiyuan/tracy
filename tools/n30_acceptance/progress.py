"""Durable heartbeat for the acceptance coordinator (not product progress)."""

from __future__ import annotations

import json
import os
from pathlib import Path
import threading
import time

from .io_utils import atomic_write_json


class AcceptanceProgress:
    def __init__(self, run_directory: Path, output_root: Path, *, interval_seconds=5.0):
        self.run_directory = Path(run_directory)
        self.output_root = Path(output_root)
        self.interval_seconds = interval_seconds
        self._start = time.monotonic_ns()
        self._lock = threading.Lock()
        self._write_lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None
        self._error = None
        self._state = {"status": "RUNNING", "phase": "initializing", "command": "",
                       "pid": os.getpid(), "run_directory": str(self.run_directory),
                       "estimated_remaining": "unavailable_until_measured"}

    @property
    def is_alive(self):
        return self._thread is not None and self._thread.is_alive()

    def update(self, **values):
        with self._lock:
            self._state.update(values)

    def write_now(self):
        with self._write_lock:
            with self._lock:
                state = dict(self._state)
            state["elapsed_ns"] = time.monotonic_ns() - self._start
            state["heartbeat_unix_ns"] = time.time_ns()
            metrics_path = state.get("metrics_path")
            if metrics_path and Path(metrics_path).is_file():
                with Path(metrics_path).open("rb") as source:
                    source.seek(max(0, source.seek(0, 2) - 16384))
                    lines = source.read().splitlines()
                for line in reversed(lines):
                    try:
                        state["latest_process_metrics"] = json.loads(line)
                        break
                    except (ValueError, UnicodeDecodeError):
                        continue
            self.run_directory.mkdir(parents=True, exist_ok=True)
            atomic_write_json(self.output_root / "acceptance-progress.json", state)
            with (self.run_directory / "progress.ndjson").open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(state, ensure_ascii=False, separators=(",", ":")) + "\n")
                stream.flush()

    def _loop(self):
        while not self._stop.wait(self.interval_seconds):
            try:
                self.write_now()
            except Exception as error:
                self._error = error
                self._stop.set()

    def __enter__(self):
        self.write_now()
        self._thread = threading.Thread(target=self._loop, name="N30AProgress", daemon=True)
        self._thread.start()
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=10)
        if exc_type is not None:
            self.update(status="INCOMPLETE", last_error=str(exc_value))
        self.write_now()
        if self._error is not None and exc_type is None:
            raise RuntimeError(f"progress persistence failed: {self._error}")
