"""Atomic phase results and strict resume identity."""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any, Mapping

from .io_utils import atomic_write_json, canonical_json_bytes, load_json
from .status import AcceptanceStatus


def compute_run_fingerprint(identity: Mapping[str, Any]) -> str:
    return hashlib.sha256(canonical_json_bytes(identity)).hexdigest()


class PhaseStore:
    def __init__(self, run_directory: Path, run_fingerprint: str):
        self.run_directory = Path(run_directory)
        self.run_fingerprint = run_fingerprint
        self.phase_directory = self.run_directory / "phases"

    def _path(self, phase: str) -> Path:
        if not phase or any(char not in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for char in phase):
            raise ValueError(f"invalid phase name: {phase!r}")
        return self.phase_directory / f"{phase}.json"

    def commit(
        self,
        *,
        phase: str,
        status: AcceptanceStatus,
        evidence: Mapping[str, Any],
    ) -> None:
        record = {
            "phase": phase,
            "run_fingerprint": self.run_fingerprint,
            "status": AcceptanceStatus(status).value,
            "evidence": dict(evidence),
        }
        atomic_write_json(self._path(phase), record)

    def load(self, phase: str) -> dict[str, Any] | None:
        path = self._path(phase)
        if not path.is_file():
            return None
        value = load_json(path)
        if not isinstance(value, dict):
            raise ValueError(f"phase record is not an object: {path}")
        return value

    def is_reusable(self, phase: str) -> bool:
        record = self.load(phase)
        return bool(
            record
            and record.get("run_fingerprint") == self.run_fingerprint
            and record.get("status") == AcceptanceStatus.PASS.value
        )
