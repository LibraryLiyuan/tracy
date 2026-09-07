"""Shared phase planning and preflight policy for N30A."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

from .status import AcceptanceStatus


PHASE_ORDER = ("P0", "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8", "P9")


@dataclass(frozen=True)
class DiskPreflight:
    volume_capacity_bytes: int
    volume_free_bytes: int
    scratch_hard_bytes: int
    required_reserve_bytes: int
    required_free_bytes: int
    passed: bool


def select_phases(
    phases: Iterable[str],
    *,
    from_phase: str | None = None,
    rerun_phase: str | None = None,
) -> tuple[str, ...]:
    values = tuple(phases)
    if from_phase is not None and rerun_phase is not None:
        raise ValueError("--from-phase and --rerun-phase are mutually exclusive")
    requested = from_phase if from_phase is not None else rerun_phase
    if requested is not None and requested not in values:
        raise ValueError(f"unknown phase: {requested}")
    if from_phase is not None:
        return values[values.index(from_phase) :]
    if rerun_phase is not None:
        return (rerun_phase,) if rerun_phase == "P9" else (rerun_phase, "P9")
    return values


def dependency_allows(
    completed_phase: str,
    status: AcceptanceStatus,
    candidate_phase: str,
) -> bool:
    status = AcceptanceStatus(status)
    if status in {AcceptanceStatus.PASS, AcceptanceStatus.DEFERRED_OUT_OF_SCOPE}:
        return True
    if candidate_phase == "P9":
        return True
    if completed_phase in {"P0", "P1", "P2"}:
        return False
    if completed_phase == "P3":
        return candidate_phase == "P5"
    if completed_phase in {"P4", "P5"} and candidate_phase == "P8":
        return False
    return False


def evaluate_disk_preflight(
    *,
    volume_capacity_bytes: int,
    volume_free_bytes: int,
    scratch_hard_bytes: int,
    minimum_reserve_bytes: int,
    minimum_reserve_percent: int,
) -> DiskPreflight:
    if min(
        volume_capacity_bytes,
        volume_free_bytes,
        scratch_hard_bytes,
        minimum_reserve_bytes,
        minimum_reserve_percent,
    ) < 0:
        raise ValueError("disk preflight values cannot be negative")
    percent_reserve = (
        volume_capacity_bytes * minimum_reserve_percent + 99
    ) // 100
    reserve = max(minimum_reserve_bytes, percent_reserve)
    required = scratch_hard_bytes + reserve
    return DiskPreflight(
        volume_capacity_bytes=volume_capacity_bytes,
        volume_free_bytes=volume_free_bytes,
        scratch_hard_bytes=scratch_hard_bytes,
        required_reserve_bytes=reserve,
        required_free_bytes=required,
        passed=volume_free_bytes >= required,
    )
