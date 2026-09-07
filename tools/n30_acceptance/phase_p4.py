"""P4 fixed 30-minute source to a fresh, fully audited Session."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping

from .acceptance_runner import PhaseOutcome
from .comparison_log import ComparisonRecorder
from .comparators import (
    ComparisonResult,
    ComparisonStatus,
    compare_at_most,
    compare_equal,
    compare_exact,
    compare_true,
)
from .evidence import parse_converter_summary
from .identity_phase import parse_session_manifest_header
from .status import AcceptanceStatus


_SESSION_STATE_NAMES = (
    "InventoryBuilding", "InventoryFailed", "CapacityPreflight",
    "CanonicalBuilding", "CanonicalPaused", "CanonicalFailed",
    "DerivedBuilding", "DerivedFailed", "FinalAuditing",
    "CancelledResumable", "Complete", "CompleteSourceDegraded",
    "InvalidSource", "InvalidConverterOutput", "InvalidChecksum",
    "InvalidCoreGap", "InvalidCapacity", "InsufficientDisk", "IdentityMismatch",
)


def _state_name(value: Any) -> str:
    return (
        _SESSION_STATE_NAMES[value]
        if isinstance(value, int) and 0 <= value < len(_SESSION_STATE_NAMES)
        else f"Unknown({value})"
    )


@dataclass(frozen=True)
class FreshConversionObservation:
    exit_code: int
    timed_out: bool
    no_progress_timed_out: bool
    peak_private_bytes: int
    session_exists: bool
    current_generation: str
    manifest: Mapping[str, Any]
    summary: Mapping[str, Any]


def evaluate_fresh_conversion(
    observation: FreshConversionObservation,
    *,
    expected: Mapping[str, Any],
    recorder: ComparisonRecorder,
) -> PhaseOutcome:
    integrity: list[ComparisonResult] = [
        compare_exact("P4.converter.exit_code", 0, observation.exit_code),
        compare_true("P4.converter.not_timed_out", not observation.timed_out),
        compare_true(
            "P4.converter.no_progress_watchdog_clear",
            not observation.no_progress_timed_out,
        ),
        compare_true("P4.session.output_exists", observation.session_exists),
        compare_equal(
            "P4.session.current_matches_manifest",
            str(observation.manifest.get("generation", "")),
            observation.current_generation,
        ),
        compare_exact(
            "P4.session.store_schema", expected["session_schema"],
            observation.manifest.get("store_schema"),
        ),
        compare_exact(
            "P4.session.canonical_schema", expected["canonical_schema"],
            observation.manifest.get("canonical_schema"),
        ),
        compare_exact(
            "P4.session.derived_schema", expected["derived_schema"],
            observation.manifest.get("derived_schema"),
        ),
        compare_exact(
            "P4.session.protocol", expected["protocol"],
            observation.manifest.get("source_protocol"),
        ),
        compare_true(
            "P4.session.mandatory_derived_complete",
            observation.manifest.get("mandatory_derived_complete") == 1,
        ),
        compare_true(
            "P4.session.audit_complete", observation.manifest.get("audit_complete") == 1
        ),
        compare_exact(
            "P4.session.state_code", expected["state_code"],
            observation.manifest.get("state"),
        ),
        compare_equal(
            "P4.session.state", str(expected["state"]),
            _state_name(observation.manifest.get("state")),
        ),
    ]
    correctness: list[ComparisonResult] = [
        compare_equal(
            "P4.session.source_sha256", str(expected["source_sha256"]).lower(),
            str(observation.manifest.get("source_sha256", "")).lower(),
        ),
        compare_exact(
            "P4.session.source_size", expected["source_size"],
            observation.manifest.get("source_size"),
        ),
        compare_equal(
            "P4.session.degradation_reason", str(expected["reason"]),
            str(observation.manifest.get("reason", "")),
        ),
        compare_exact(
            "P4.summary.canonical_shards",
            observation.manifest.get("shard_count"),
            observation.summary.get("session.canonical_shards"),
        ),
    ]
    performance = [
        compare_at_most(
            "P4.converter.peak_private_bytes",
            expected["hard_private_bytes"],
            observation.peak_private_bytes,
        )
    ]
    for result in (*integrity, *correctness, *performance):
        recorder.record(result)
    integrity_failures = [r.name for r in integrity if r.status == ComparisonStatus.FAIL]
    correctness_failures = [r.name for r in correctness if r.status == ComparisonStatus.FAIL]
    performance_failures = [r.name for r in performance if r.status == ComparisonStatus.FAIL]
    evidence = {
        "failed_comparisons": integrity_failures + correctness_failures + performance_failures,
        "summary": dict(observation.summary),
        "peak_private_bytes": str(observation.peak_private_bytes),
    }
    if integrity_failures:
        return PhaseOutcome(
            "P4", AcceptanceStatus.FAIL_INTEGRITY,
            f"fresh Session publication/integrity failed ({len(integrity_failures)} comparisons)",
            repair_direction="inspect converter logs, building generation and Final Audit failure",
            evidence=evidence,
        )
    if correctness_failures:
        return PhaseOutcome(
            "P4", AcceptanceStatus.FAIL_CORRECTNESS,
            f"fresh Session identity/state facts differ ({len(correctness_failures)} comparisons)",
            repair_direction="inspect source binding, degradation propagation and converter summary",
            evidence=evidence,
        )
    if performance_failures:
        return PhaseOutcome(
            "P4", AcceptanceStatus.FAIL_PERFORMANCE,
            "fresh 30-minute conversion exceeded the 16 GiB private-memory hard gate",
            repair_direction="profile the converter stage responsible for the memory high-water mark",
            evidence=evidence,
        )
    return PhaseOutcome(
        "P4", AcceptanceStatus.PASS,
        "fresh 30-minute Session published with matching identity and completed Final Audit",
        evidence=evidence,
    )


def run_p4(context) -> PhaseOutcome:
    target = context.inputs.fresh_session_30m
    target_absent = not target.exists() and not any(
        target.parent.glob(target.name + ".building.*")
    )
    absent_check = context.recorder.record(
        compare_true("P4.target.initially_absent", target_absent)
    )
    if absent_check.status == ComparisonStatus.FAIL:
        return PhaseOutcome(
            "P4", AcceptanceStatus.BLOCKED_ENVIRONMENT,
            "fresh Session target or a prior building generation already exists",
            repair_direction="choose a new empty fresh_session_30m path; do not overwrite evidence",
            evidence={"target": str(target), "failed_comparisons": [absent_check.name]},
        )
    target.parent.mkdir(parents=True, exist_ok=True)
    converter = context.inputs.build_capture / "Release" / "tracy-stream-convert.exe"
    result, stdout, _ = context.commands.run_text(
        "P4-fresh-30m-session-convert",
        (str(converter), "-i", str(context.inputs.source_30m), "-o", str(target)),
        cwd=target.parent,
        timeout_seconds=24 * 60 * 60,
        no_progress_timeout_seconds=float(
            context.profile.get("converter", {}).get("no_progress_timeout_seconds", 120)
        ),
    )
    summary: Mapping[str, Any] = {}
    try:
        summary = parse_converter_summary(stdout)
    except ValueError:
        pass
    current = ""
    manifest: Mapping[str, Any] = {}
    if target.is_dir() and (target / "CURRENT").is_file():
        current = (target / "CURRENT").read_text(encoding="utf-8").strip()
        manifest_path = target / "generations" / current / "manifest"
        if manifest_path.is_file():
            manifest = parse_session_manifest_header(
                manifest_path.read_text(encoding="utf-8")
            )
    golden = context.golden
    if not isinstance(golden, Mapping):
        raise ValueError("P4 requires Golden")
    expected = {
        "source_sha256": golden["source"]["sha256"],
        "source_size": golden["source"]["size"],
        "protocol": context.profile["protocol"],
        "session_schema": context.profile["session_schema"],
        "canonical_schema": context.profile["canonical_schema"],
        "derived_schema": context.profile["derived_schema"],
        "state_code": context.profile["expected_session_state_code"],
        "state": golden["expected_state"],
        "reason": golden["expected_degradation_reason"],
        "hard_private_bytes": context.profile["converter"]["hard_private_bytes"],
    }
    outcome = evaluate_fresh_conversion(
        FreshConversionObservation(
            exit_code=result.exit_code,
            timed_out=result.timed_out,
            no_progress_timed_out=result.no_progress_timed_out,
            peak_private_bytes=result.peak_private_bytes,
            session_exists=target.is_dir(),
            current_generation=current,
            manifest=manifest,
            summary=summary,
        ),
        expected=expected,
        recorder=context.recorder,
    )
    return PhaseOutcome(
        "P4", outcome.status, outcome.summary,
        repair_direction=outcome.repair_direction,
        evidence={**dict(outcome.evidence), "session": str(target)},
    )
