"""Safe subprocess execution with durable logs and process-tree metrics."""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from typing import Sequence

from .win32_metrics import (
    ProcessIdentity,
    ProcessMetricError,
    capture_process_identity,
    same_process,
    sample_process_tree,
)


@dataclass(frozen=True)
class ProcessResult:
    command: tuple[str, ...]
    pid: int
    identity: ProcessIdentity | None
    exit_code: int
    elapsed_ns: int
    timed_out: bool
    wall_clock_timed_out: bool
    no_progress_timed_out: bool
    sample_count: int
    peak_working_set_bytes: int
    peak_private_bytes: int
    read_bytes: int
    write_bytes: int
    console_break_count: int = 0
    first_console_break_elapsed_ns: int = 0


@dataclass(frozen=True)
class ConsoleBreakPlan:
    output_substring: str
    second_break_delay_seconds: float | None = None

    def __post_init__(self) -> None:
        if not self.output_substring:
            raise ValueError("output_substring must not be empty")
        if self.second_break_delay_seconds is not None and self.second_break_delay_seconds < 0:
            raise ValueError("second_break_delay_seconds cannot be negative")


class ProcessRunner:
    def __init__(self, *, sample_interval_seconds: float = 1.0):
        if sample_interval_seconds <= 0:
            raise ValueError("sample_interval_seconds must be positive")
        self.sample_interval_seconds = sample_interval_seconds

    @staticmethod
    def _capture_identity(pid: int) -> ProcessIdentity | None:
        for _ in range(10):
            try:
                return capture_process_identity(pid)
            except ProcessMetricError:
                time.sleep(0.005)
        return None

    @staticmethod
    def _terminate_verified(process: subprocess.Popen, identity: ProcessIdentity | None) -> None:
        if process.poll() is not None:
            return
        if identity is None or not same_process(identity):
            raise RuntimeError(
                "refusing to terminate process because PID/creation-time/image identity is unavailable or changed"
            )
        process.kill()

    @staticmethod
    def _console_break_verified(
        process: subprocess.Popen, identity: ProcessIdentity | None
    ) -> None:
        if process.poll() is not None:
            return
        if identity is None or not same_process(identity):
            raise RuntimeError(
                "refusing to signal process because PID/creation-time/image identity is unavailable or changed"
            )
        if sys.platform != "win32":
            raise RuntimeError("Console Break control is only available on Windows")
        os.kill(process.pid, signal.CTRL_BREAK_EVENT)

    def run(
        self,
        command: Sequence[str],
        *,
        cwd: Path,
        stdout_path: Path,
        stderr_path: Path,
        metrics_path: Path,
        timeout_seconds: float | None = None,
        no_progress_timeout_seconds: float | None = None,
        echo_output: bool = True,
        console_break_plan: ConsoleBreakPlan | None = None,
        outer_job_bytes: int | None = None,
    ) -> ProcessResult:
        if not command:
            raise ValueError("command is empty")
        cwd = Path(cwd)
        stdout_path = Path(stdout_path)
        stderr_path = Path(stderr_path)
        metrics_path = Path(metrics_path)
        for path in (stdout_path, stderr_path, metrics_path):
            path.parent.mkdir(parents=True, exist_ok=True)
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == "win32" else 0
        start = time.monotonic_ns()
        print(
            f"[PROCESS][START] exe={command[0]} cwd={cwd} metrics={metrics_path}",
            flush=True,
        )
        peak_working_set = 0
        peak_private = 0
        read_bytes = 0
        write_bytes = 0
        samples = 0
        timed_out = False
        wall_clock_timed_out = False
        no_progress_timed_out = False
        last_activity_ns = start
        observed_sizes = {stdout_path: 0, stderr_path: 0}
        echo_offsets = {stdout_path: 0, stderr_path: 0}
        control_text = ""
        console_break_count = 0
        first_console_break_ns = 0

        def observe_output() -> None:
            nonlocal last_activity_ns, control_text
            for path, label in ((stdout_path, "stdout"), (stderr_path, "stderr")):
                try:
                    size = path.stat().st_size
                except FileNotFoundError:
                    continue
                if size > observed_sizes[path]:
                    observed_sizes[path] = size
                    last_activity_ns = time.monotonic_ns()
                if (not echo_output and console_break_plan is None) or size <= echo_offsets[path]:
                    continue
                with path.open("rb") as source:
                    source.seek(echo_offsets[path])
                    payload = source.read(size - echo_offsets[path])
                echo_offsets[path] = size
                text = payload.decode("utf-8", errors="replace")
                if console_break_plan is not None:
                    control_text = (control_text + text)[-16384:]
                if echo_output:
                    for line in text.splitlines():
                        print(f"[TOOL][{label}] {line}", flush=True)

        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            process = subprocess.Popen(
                tuple(str(value) for value in command),
                cwd=str(cwd),
                stdin=subprocess.DEVNULL,
                stdout=stdout,
                stderr=stderr,
                shell=False,
                creationflags=creationflags,
            )
            identity = self._capture_identity(process.pid)
            job_limit = None
            if outer_job_bytes is not None:
                from .win32_job_limit import ProcessJobLimit
                try:
                    job_limit = ProcessJobLimit(process.pid, outer_job_bytes)
                except Exception:
                    self._terminate_verified(process, identity)
                    process.wait(timeout=5)
                    raise
            deadline_ns = (
                start + int(timeout_seconds * 1_000_000_000)
                if timeout_seconds is not None
                else None
            )
            next_sample = 0
            with metrics_path.open("w", encoding="utf-8", newline="\n") as metrics:
                while True:
                    now = time.monotonic_ns()
                    observe_output()
                    if (
                        console_break_plan is not None
                        and console_break_count == 0
                        and console_break_plan.output_substring in control_text
                        and process.poll() is None
                    ):
                        self._console_break_verified(process, identity)
                        console_break_count = 1
                        first_console_break_ns = now
                        print(
                            f"[PROCESS][CTRL_BREAK] pid={process.pid} count=1 "
                            f"trigger={console_break_plan.output_substring}",
                            flush=True,
                        )
                    if (
                        console_break_plan is not None
                        and console_break_plan.second_break_delay_seconds is not None
                        and console_break_count == 1
                        and now - first_console_break_ns
                        >= int(console_break_plan.second_break_delay_seconds * 1_000_000_000)
                        and process.poll() is None
                    ):
                        self._console_break_verified(process, identity)
                        console_break_count = 2
                        print(
                            f"[PROCESS][CTRL_BREAK] pid={process.pid} count=2",
                            flush=True,
                        )
                    if now >= next_sample and process.poll() is None:
                        try:
                            sample = sample_process_tree(process.pid)
                        except ProcessMetricError as error:
                            metrics.write(
                                json.dumps(
                                    {
                                        "timestamp_monotonic_ns": str(now),
                                        "metric_error": str(error),
                                    },
                                    separators=(",", ":"),
                                )
                                + "\n"
                            )
                        else:
                            metrics.write(
                                json.dumps(
                                    sample.as_dict(),
                                    sort_keys=True,
                                    separators=(",", ":"),
                                )
                                + "\n"
                            )
                            samples += 1
                            peak_working_set = max(
                                peak_working_set,
                                sample.working_set_bytes,
                                sample.peak_working_set_bytes,
                            )
                            peak_private = max(
                                peak_private,
                                sample.private_bytes,
                                sample.peak_private_bytes,
                            )
                            read_bytes = max(read_bytes, sample.read_bytes)
                            write_bytes = max(write_bytes, sample.write_bytes)
                        metrics.flush()
                        next_sample = now + int(self.sample_interval_seconds * 1_000_000_000)
                    exit_code = process.poll()
                    if exit_code is not None:
                        break
                    if deadline_ns is not None and now >= deadline_ns:
                        timed_out = True
                        wall_clock_timed_out = True
                        self._terminate_verified(process, identity)
                        exit_code = process.wait(timeout=5)
                        break
                    if (
                        no_progress_timeout_seconds is not None
                        and now - last_activity_ns
                        >= int(no_progress_timeout_seconds * 1_000_000_000)
                    ):
                        timed_out = True
                        no_progress_timed_out = True
                        self._terminate_verified(process, identity)
                        exit_code = process.wait(timeout=5)
                        break
                    time.sleep(min(self.sample_interval_seconds / 4, 0.025))
            stdout.flush()
            stderr.flush()
        observe_output()
        if job_limit is not None:
            job_limit.close()
        elapsed = time.monotonic_ns() - start
        print(
            f"[PROCESS][END] pid={process.pid} exit={exit_code} elapsed_ns={elapsed} "
            f"peak_private_bytes={peak_private} timed_out={str(timed_out).lower()}",
            flush=True,
        )
        return ProcessResult(
            command=tuple(str(value) for value in command),
            pid=process.pid,
            identity=identity,
            exit_code=int(exit_code),
            elapsed_ns=elapsed,
            timed_out=timed_out,
            wall_clock_timed_out=wall_clock_timed_out,
            no_progress_timed_out=no_progress_timed_out,
            sample_count=samples,
            peak_working_set_bytes=peak_working_set,
            peak_private_bytes=peak_private,
            read_bytes=read_bytes,
            write_bytes=write_bytes,
            console_break_count=console_break_count,
            first_console_break_elapsed_ns=(
                first_console_break_ns - start if first_console_break_ns else 0
            ),
        )
