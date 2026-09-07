"""Audited command execution with one artifact directory per invocation."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
from typing import Any, Sequence

from .process_runner import ConsoleBreakPlan, ProcessRunner, ProcessResult


_SAFE_LABEL = re.compile(r"[^A-Za-z0-9_.-]+")


class CommandExecutor:
    def __init__(self, run_directory: Path, *, runner: ProcessRunner | None = None, progress=None):
        self.progress = progress
        self.outer_job_bytes = None
        self.run_directory = Path(run_directory)
        self.run_directory.mkdir(parents=True, exist_ok=True)
        self.raw_directory = self.run_directory / "raw"
        self.raw_directory.mkdir(parents=True, exist_ok=True)
        self.command_log = self.run_directory / "commands.ndjson"
        self.runner = runner if runner is not None else ProcessRunner()
        self.sequence = 0
        if self.command_log.is_file():
            with self.command_log.open("r", encoding="utf-8") as stream:
                for line_number, line in enumerate(stream, 1):
                    if not line.strip():
                        continue
                    record = json.loads(line)
                    expected_sequence = self.sequence + 1
                    if not isinstance(record, dict) or record.get("sequence") != expected_sequence:
                        raise ValueError(
                            f"command log sequence gap at line {line_number}"
                        )
                    self.sequence = expected_sequence

    def run(
        self,
        label: str,
        command: Sequence[str],
        *,
        cwd: Path,
        timeout_seconds: float | None = None,
        no_progress_timeout_seconds: float | None = None,
        echo_output: bool = True,
        console_break_plan: ConsoleBreakPlan | None = None,
    ) -> ProcessResult:
        self.sequence += 1
        safe_label = _SAFE_LABEL.sub("-", label).strip("-") or "command"
        artifacts = self.raw_directory / f"{self.sequence:03d}-{safe_label}"
        artifacts.mkdir(parents=True, exist_ok=False)
        from .io_utils import atomic_write_json
        atomic_write_json(artifacts / "invocation.json", {
            "sequence": self.sequence, "label": label,
            "command": list(command), "cwd": str(cwd), "state": "started",
        })
        if self.progress is not None:
            self.progress.update(command=label, executable=str(command[0]),
                                 metrics_path=str(artifacts / "metrics.ndjson"))
        result = self.runner.run(
            tuple(str(value) for value in command),
            cwd=Path(cwd),
            stdout_path=artifacts / "stdout.log",
            stderr_path=artifacts / "stderr.log",
            metrics_path=artifacts / "metrics.ndjson",
            timeout_seconds=timeout_seconds,
            no_progress_timeout_seconds=no_progress_timeout_seconds,
            echo_output=echo_output,
            console_break_plan=console_break_plan,
            outer_job_bytes=self.outer_job_bytes if Path(command[0]).name.lower() == "tracy-stream-convert.exe" else None,
        )
        record: dict[str, Any] = {
            "sequence": self.sequence,
            "label": label,
            "command": [str(value) for value in command],
            "cwd": str(Path(cwd).resolve()),
            "artifacts": str(artifacts.resolve()),
            "exit_code": result.exit_code,
            "elapsed_ns": str(result.elapsed_ns),
            "peak_private_bytes": str(result.peak_private_bytes),
            "console_break_count": result.console_break_count,
            "pid": result.pid,
            "timed_out": result.timed_out,
            "no_progress_timed_out": result.no_progress_timed_out,
            "peak_working_set_bytes": str(result.peak_working_set_bytes),
            "read_bytes": str(result.read_bytes),
            "write_bytes": str(result.write_bytes),
            "sample_count": result.sample_count,
        }
        with self.command_log.open("a", encoding="utf-8", newline="\n") as stream:
            stream.write(
                json.dumps(
                    record,
                    ensure_ascii=False,
                    sort_keys=True,
                    separators=(",", ":"),
                    allow_nan=False,
                )
                + "\n"
            )
            stream.flush()
            os.fsync(stream.fileno())
        return result

    def run_text(
        self,
        label: str,
        command: Sequence[str],
        *,
        cwd: Path,
        timeout_seconds: float | None = None,
        no_progress_timeout_seconds: float | None = None,
        echo_output: bool = True,
    ) -> tuple[ProcessResult, str, str]:
        result = self.run(
            label,
            command,
            cwd=cwd,
            timeout_seconds=timeout_seconds,
            no_progress_timeout_seconds=no_progress_timeout_seconds,
            echo_output=echo_output,
        )
        safe_label = _SAFE_LABEL.sub("-", label).strip("-") or "command"
        artifacts = self.raw_directory / f"{self.sequence:03d}-{safe_label}"
        stdout = (artifacts / "stdout.log").read_text(
            encoding="utf-8", errors="replace"
        )
        stderr = (artifacts / "stderr.log").read_text(
            encoding="utf-8", errors="replace"
        )
        return result, stdout, stderr
