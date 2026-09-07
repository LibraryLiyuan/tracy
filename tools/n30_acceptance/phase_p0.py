"""P0 identity and capacity gate."""

from __future__ import annotations

from dataclasses import dataclass
import shutil
from pathlib import Path
from typing import Any, Mapping

from .acceptance_runner import PhaseOutcome
from .comparison_log import ComparisonRecorder
from .comparators import (
    ComparisonResult,
    ComparisonStatus,
    compare_at_least,
    compare_equal,
    compare_exact,
    compare_true,
)
from .identity_phase import GitPorcelainIdentity
from .identity_phase import parse_git_porcelain_v2, parse_session_manifest_header
from .io_utils import sha256_file
from .orchestrator import evaluate_disk_preflight
from .status import AcceptanceStatus


@dataclass(frozen=True)
class P0Snapshot:
    git: GitPorcelainIdentity
    baseline_is_ancestor: bool
    source_size: int
    source_sha256: str
    short_stream_exists: bool
    short_trace_exists: bool
    current_generation: str
    manifest: Mapping[str, Any]
    volume_capacity_bytes: int
    volume_free_bytes: int


_SESSION_STATE_NAMES = (
    "InventoryBuilding",
    "InventoryFailed",
    "CapacityPreflight",
    "CanonicalBuilding",
    "CanonicalPaused",
    "CanonicalFailed",
    "DerivedBuilding",
    "DerivedFailed",
    "FinalAuditing",
    "CancelledResumable",
    "Complete",
    "CompleteSourceDegraded",
    "InvalidSource",
    "InvalidConverterOutput",
    "InvalidChecksum",
    "InvalidCoreGap",
    "InvalidCapacity",
    "InsufficientDisk",
    "IdentityMismatch",
)


def _session_state_name(value: Any) -> str:
    if isinstance(value, int) and 0 <= value < len(_SESSION_STATE_NAMES):
        return _SESSION_STATE_NAMES[value]
    return f"Unknown({value})"


def evaluate_p0_snapshot(
    snapshot: P0Snapshot,
    *,
    expected: Mapping[str, Any],
    recorder: ComparisonRecorder,
) -> PhaseOutcome:
    manifest = snapshot.manifest
    disk = evaluate_disk_preflight(
        volume_capacity_bytes=snapshot.volume_capacity_bytes,
        volume_free_bytes=snapshot.volume_free_bytes,
        scratch_hard_bytes=int(expected["scratch_hard_bytes"]),
        minimum_reserve_bytes=int(expected["minimum_reserve_bytes"]),
        minimum_reserve_percent=int(expected["minimum_reserve_percent"]),
    )
    checks: list[ComparisonResult] = [
        compare_equal("P0.git.branch", str(expected["branch"]), snapshot.git.branch),
        compare_true("P0.git.required_ancestor", snapshot.baseline_is_ancestor),
        compare_equal("P0.git.tracked_dirty", (), snapshot.git.tracked_dirty),
        compare_exact("P0.source.size", expected["source_size"], snapshot.source_size),
        compare_equal(
            "P0.source.sha256",
            str(expected["source_sha256"]).lower(),
            snapshot.source_sha256.lower(),
        ),
        compare_true("P0.short_stream.exists", snapshot.short_stream_exists),
        compare_true("P0.short_trace.exists", snapshot.short_trace_exists),
        compare_equal(
            "P0.session.current_matches_manifest",
            str(manifest.get("generation", "")),
            snapshot.current_generation,
        ),
        compare_exact("P0.session.store_schema", expected["session_schema"], manifest.get("store_schema")),
        compare_exact("P0.session.canonical_schema", expected["canonical_schema"], manifest.get("canonical_schema")),
        compare_exact("P0.session.derived_schema", expected["derived_schema"], manifest.get("derived_schema")),
        compare_exact("P0.session.protocol", expected["protocol"], manifest.get("source_protocol")),
        compare_equal(
            "P0.session.source_sha256",
            str(expected["source_sha256"]).lower(),
            str(manifest.get("source_sha256", "")).lower(),
        ),
        compare_exact("P0.session.source_size", expected["source_size"], manifest.get("source_size")),
        compare_exact("P0.session.state_code", expected["session_state_code"], manifest.get("state")),
        compare_equal(
            "P0.session.state",
            str(expected["session_state"]),
            _session_state_name(manifest.get("state")),
        ),
        compare_true(
            "P0.session.mandatory_derived_complete",
            manifest.get("mandatory_derived_complete") == 1,
        ),
        compare_true("P0.session.audit_complete", manifest.get("audit_complete") == 1),
        compare_equal(
            "P0.session.degradation_reason",
            str(expected["degradation_reason"]),
            str(manifest.get("reason", "")),
        ),
        compare_exact(
            "P0.session.canonical_shards",
            expected["canonical_shards"],
            manifest.get("shard_count"),
        ),
        compare_at_least(
            "P0.disk.free_bytes", disk.required_free_bytes, disk.volume_free_bytes
        ),
    ]
    for check in checks:
        recorder.record(check)
    failed = [check.name for check in checks if check.status == ComparisonStatus.FAIL]
    if failed:
        return PhaseOutcome(
            "P0",
            AcceptanceStatus.BLOCKED_ENVIRONMENT,
            f"identity or capacity gate failed ({len(failed)} comparisons)",
            repair_direction="correct the fixed input/repository/session identity or free disk space",
            evidence={"failed_comparisons": failed, "disk": disk.__dict__},
        )
    return PhaseOutcome(
        "P0",
        AcceptanceStatus.PASS,
        f"all {len(checks)} identity and capacity comparisons passed",
        evidence={
            "git_head": snapshot.git.head,
            "git_ahead": snapshot.git.ahead,
            "git_behind": snapshot.git.behind,
            "untracked": list(snapshot.git.untracked),
            "disk": disk.__dict__,
        },
    )


def run_p0(context) -> PhaseOutcome:
    if context.golden is None:
        raise ValueError("P0 requires a loaded Golden oracle")
    profile = context.profile
    golden = context.golden
    inputs = context.inputs
    git_profile = profile.get("git")
    disk_profile = profile.get("disk")
    if not isinstance(git_profile, Mapping) or not isinstance(disk_profile, Mapping):
        raise ValueError("profile is missing git or disk policy")

    git_command = (
        "git",
        "--no-optional-locks",
        "-c",
        f"safe.directory={inputs.worktree.as_posix()}",
        "status",
        "--porcelain=v2",
        "--branch",
        "--untracked-files=all",
    )
    git_result, git_stdout, _ = context.commands.run_text(
        "P0-git-status", git_command, cwd=inputs.worktree
    )
    if git_result.exit_code != 0:
        raise RuntimeError(f"git status failed with exit code {git_result.exit_code}")
    git_identity = parse_git_porcelain_v2(git_stdout)

    ancestor_result = context.commands.run(
        "P0-git-required-ancestor",
        (
            "git",
            "-c",
            f"safe.directory={inputs.worktree.as_posix()}",
            "merge-base",
            "--is-ancestor",
            str(git_profile["required_ancestor"]),
            "HEAD",
        ),
        cwd=inputs.worktree,
    )
    if ancestor_result.exit_code not in (0, 1):
        raise RuntimeError(
            f"git merge-base failed with exit code {ancestor_result.exit_code}"
        )

    source_size = inputs.source_30m.stat().st_size
    print(
        f"[HASH][START] P0.source.sha256 path={inputs.source_30m} bytes={source_size}",
        flush=True,
    )
    source_sha256 = sha256_file(inputs.source_30m)
    print(f"[HASH][END] P0.source.sha256 sha256={source_sha256}", flush=True)

    current = (inputs.existing_session_30m / "CURRENT").read_text(
        encoding="utf-8"
    ).strip()
    manifest = parse_session_manifest_header(
        (inputs.existing_session_30m / "manifest").read_text(encoding="utf-8")
    )
    disk = shutil.disk_usage(inputs.scratch_root)
    expected = {
        "branch": git_profile["branch"],
        "source_size": golden["source"]["size"],
        "source_sha256": golden["source"]["sha256"],
        "protocol": profile["protocol"],
        "session_schema": profile["session_schema"],
        "canonical_schema": profile["canonical_schema"],
        "derived_schema": profile["derived_schema"],
        "session_state_code": profile["expected_session_state_code"],
        "session_state": golden["expected_state"],
        "degradation_reason": golden["expected_degradation_reason"],
        "canonical_shards": golden["metrics"]["session.canonical_shards"],
        "scratch_hard_bytes": disk_profile["scratch_hard_bytes"],
        "minimum_reserve_bytes": disk_profile["minimum_free_reserve_bytes"],
        "minimum_reserve_percent": disk_profile["minimum_free_reserve_percent"],
    }
    snapshot = P0Snapshot(
        git=git_identity,
        baseline_is_ancestor=ancestor_result.exit_code == 0,
        source_size=source_size,
        source_sha256=source_sha256,
        short_stream_exists=inputs.short_stream.is_file(),
        short_trace_exists=inputs.short_trace.is_file(),
        current_generation=current,
        manifest=manifest,
        volume_capacity_bytes=disk.total,
        volume_free_bytes=disk.free,
    )
    return evaluate_p0_snapshot(snapshot, expected=expected, recorder=context.recorder)
