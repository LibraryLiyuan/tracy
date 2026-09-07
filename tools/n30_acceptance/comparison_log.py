"""Immediate human and NDJSON logging for every acceptance comparison."""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys
from typing import Any, TextIO

from .comparators import ComparisonResult, ComparisonStatus


def _render_value(value: Any) -> str:
    if (
        isinstance(value, dict)
        and set(value) == {"numerator", "denominator"}
    ):
        return f"{value['numerator']}/{value['denominator']}"
    if isinstance(value, (dict, list, tuple)):
        return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return str(value)


class ComparisonRecorder:
    """Write each decision before the harness can advance to the next one."""

    def __init__(self, output: Path, *, console: TextIO | None = None):
        self.output = Path(output)
        self.output.parent.mkdir(parents=True, exist_ok=True)
        self.console = console if console is not None else sys.stdout
        self.sequence = 0
        self.passed = 0
        self.failed = 0
        if self.output.is_file():
            with self.output.open("r", encoding="utf-8") as stream:
                for line_number, line in enumerate(stream, 1):
                    if not line.strip():
                        continue
                    record = json.loads(line)
                    expected_sequence = self.sequence + 1
                    if not isinstance(record, dict) or record.get("sequence") != expected_sequence:
                        raise ValueError(
                            f"comparison log sequence gap at line {line_number}"
                        )
                    status = record.get("status")
                    if status == ComparisonStatus.PASS.value:
                        self.passed += 1
                    elif status == ComparisonStatus.FAIL.value:
                        self.failed += 1
                    else:
                        raise ValueError(
                            f"comparison log has invalid status at line {line_number}"
                        )
                    self.sequence = expected_sequence

    def record(self, result: ComparisonResult) -> ComparisonResult:
        self.sequence += 1
        if result.status == ComparisonStatus.PASS:
            self.passed += 1
        else:
            self.failed += 1
        record = {"sequence": self.sequence, **result.as_dict()}
        encoded = (
            json.dumps(
                record,
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
                allow_nan=False,
            )
            + "\n"
        )
        with self.output.open("a", encoding="utf-8", newline="\n") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        line = (
            f"[COMPARE] name={result.name}"
            f" | rule={result.rule}"
            f" | expected={_render_value(result.expected)}"
            f" | actual={_render_value(result.actual)}"
            f" | delta={_render_value(result.delta)}"
            f" | status={result.status.value}"
        )
        if result.detail:
            line += f" | detail={result.detail}"
        print(line, file=self.console, flush=True)
        return result

    def checkpoint(self) -> tuple[int, int]:
        return self.passed, self.failed

    def print_summary(
        self, phase: str, *, since: tuple[int, int] | None = None
    ) -> dict[str, int]:
        start_passed, start_failed = since if since is not None else (0, 0)
        if start_passed > self.passed or start_failed > self.failed:
            raise ValueError("comparison checkpoint is ahead of recorder state")
        passed = self.passed - start_passed
        failed = self.failed - start_failed
        summary = {
            "total": passed + failed,
            "passed": passed,
            "failed": failed,
        }
        print(
            f"[COMPARE-SUMMARY][{phase}] total={summary['total']} "
            f"passed={summary['passed']} failed={summary['failed']}",
            file=self.console,
            flush=True,
        )
        return summary
