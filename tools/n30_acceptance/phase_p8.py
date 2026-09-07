"""P8 bounded two-hour and 1x/2x/4x Session scale acceptance."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import Any, Mapping, Sequence

from .acceptance_runner import PhaseOutcome
from .comparison_log import ComparisonRecorder
from .comparators import (
    ComparisonResult,
    ComparisonStatus,
    compare_at_least,
    compare_at_most,
    compare_exact,
    compare_true,
    parse_exact_int,
)
from .io_utils import atomic_write_json
from .status import AcceptanceStatus


_REPORT_INTEGER_FIELDS = frozenset(
    {
        "schema",
        "factor",
        "logical_duration_ns",
        "maximum_frame_id",
        "data_shards",
        "source_protocol_events",
        "canonical_protocol_events",
        "payload_records",
        "source_bytes",
        "session_bytes",
        "temporary_bytes",
        "core_build_elapsed_ns",
        "peak_private_bytes",
        "core_gap_count",
    }
)


def parse_corpus_report(payload: str) -> dict[str, Any]:
    """Parse one machine report without allowing lossy integer conversion."""

    lines = [line for line in payload.splitlines() if line.strip()]
    if len(lines) != 1:
        raise ValueError(f"expected exactly one synthetic report, got {len(lines)}")
    value = json.loads(lines[0])
    if not isinstance(value, dict):
        raise ValueError("unsupported or malformed synthetic report")
    try:
        schema = parse_exact_int(value.get("schema"))
    except (TypeError, ValueError) as error:
        raise ValueError("unsupported or malformed synthetic report") from error
    if schema != 1:
        raise ValueError("unsupported or malformed synthetic report")
    converted: dict[str, Any] = {}
    for key, item in value.items():
        converted[key] = parse_exact_int(item) if key in _REPORT_INTEGER_FIELDS else item
    return converted


@dataclass(frozen=True)
class DurationObservation:
    generator_exit_code: int
    converter_exit_code: int
    inspector_exit_code: int
    logical_duration_ns: int
    maximum_frame_id: int
    data_shards: int
    source_protocol_events: int
    canonical_protocol_events: int
    final_audit_complete: bool
    zone_cross_shard: bool
    job_cross_shard: bool
    allocation_cross_shard: bool
    resource_cross_shard: bool
    io_cross_shard: bool
    peak_private_bytes: int
    generator_report_valid: bool = True
    inspector_report_valid: bool = True
    expected_maximum_frame_id: int | None = None


@dataclass(frozen=True)
class DensityObservation:
    factor: int
    generator_exit_code: int
    converter_exit_code: int
    inspector_exit_code: int
    payload_records: int
    source_protocol_events: int
    canonical_protocol_events: int
    source_bytes: int
    session_bytes: int
    temporary_bytes: int
    data_shards: int
    core_build_elapsed_ns: int
    peak_private_bytes: int
    final_audit_complete: bool
    core_gap_count: int
    generator_report_valid: bool = True
    inspector_report_valid: bool = True


def _measurement(name: str, actual: int) -> ComparisonResult:
    return ComparisonResult(
        name=name,
        status=ComparisonStatus.PASS,
        expected="measurement",
        actual=parse_exact_int(actual),
        delta="recorded",
        rule="measurement_only",
    )


def _compare_ratio_at_most(
    name: str,
    *,
    maximum_numerator: int,
    maximum_denominator: int,
    actual_numerator: int,
    actual_denominator: int,
) -> ComparisonResult:
    max_num = parse_exact_int(maximum_numerator)
    max_den = parse_exact_int(maximum_denominator)
    actual_num = parse_exact_int(actual_numerator)
    actual_den = parse_exact_int(actual_denominator)
    if min(max_num, max_den, actual_num, actual_den) <= 0:
        return ComparisonResult(
            name=name,
            status=ComparisonStatus.FAIL,
            expected={"at_most": {"numerator": max_num, "denominator": max_den}},
            actual={"numerator": actual_num, "denominator": actual_den},
            delta="measurement unavailable",
            rule="rational_at_most_cross_product",
            detail="growth-ratio operands must all be positive",
        )
    delta = actual_num * max_den - max_num * actual_den
    return ComparisonResult(
        name=name,
        status=ComparisonStatus.PASS if delta <= 0 else ComparisonStatus.FAIL,
        expected={"at_most": {"numerator": max_num, "denominator": max_den}},
        actual={"numerator": actual_num, "denominator": actual_den},
        delta=delta,
        rule="rational_at_most_cross_product",
    )


def evaluate_synthetic_scale(
    duration: DurationObservation,
    density: Sequence[DensityObservation],
    *,
    expected_duration_ns: int,
    minimum_shards: int,
    maximum_growth_numerator: int,
    maximum_growth_denominator: int,
    hard_private_bytes: int,
    recorder: ComparisonRecorder,
    minimum_density_shards: int = 1,
) -> PhaseOutcome:
    harness: list[ComparisonResult] = []
    correctness: list[ComparisonResult] = []
    performance: list[ComparisonResult] = []

    def record(target: list[ComparisonResult], result: ComparisonResult) -> None:
        target.append(recorder.record(result))

    for label, value in (
        ("generator", duration.generator_exit_code),
        ("converter", duration.converter_exit_code),
        ("inspector", duration.inspector_exit_code),
    ):
        record(harness, compare_exact(f"P8.duration.{label}.exit_code", 0, value))
    record(
        harness,
        compare_true("P8.duration.generator.machine_report_valid", duration.generator_report_valid),
    )
    record(
        harness,
        compare_true("P8.duration.inspector.machine_report_valid", duration.inspector_report_valid),
    )
    record(
        correctness,
        compare_exact(
            "P8.duration.logical_duration_ns", expected_duration_ns,
            duration.logical_duration_ns,
        ),
    )
    record(
        correctness,
        compare_at_least("P8.duration.maximum_frame_id", 2**32, duration.maximum_frame_id),
    )
    if duration.expected_maximum_frame_id is not None:
        record(correctness, compare_exact("P8.duration.maximum_frame_id.generator_vs_session",
            duration.expected_maximum_frame_id, duration.maximum_frame_id))
    record(
        correctness,
        compare_at_least("P8.duration.data_shards", minimum_shards, duration.data_shards),
    )
    record(
        correctness,
        compare_exact(
            "P8.duration.protocol_events.inventory_vs_canonical",
            duration.source_protocol_events,
            duration.canonical_protocol_events,
        ),
    )
    record(correctness, compare_true("P8.duration.final_audit_complete", duration.final_audit_complete))
    for name, value in (
        ("zone", duration.zone_cross_shard),
        ("job", duration.job_cross_shard),
        ("allocation", duration.allocation_cross_shard),
        ("resource", duration.resource_cross_shard),
        ("io", duration.io_cross_shard),
    ):
        record(correctness, compare_true(f"P8.duration.lifecycle.{name}_cross_shard", value))
    record(
        performance,
        compare_at_most(
            "P8.duration.peak_private_bytes", hard_private_bytes,
            duration.peak_private_bytes,
        ),
    )

    by_factor = {item.factor: item for item in density}
    factors_exact = len(by_factor) == len(density) and tuple(sorted(by_factor)) == (1, 2, 4)
    record(harness, compare_true("P8.density.factors_exact_1_2_4", factors_exact))
    if factors_exact:
        baseline = by_factor[1]
        for factor in (1, 2, 4):
            item = by_factor[factor]
            for label, value in (
                ("generator", item.generator_exit_code),
                ("converter", item.converter_exit_code),
                ("inspector", item.inspector_exit_code),
            ):
                record(harness, compare_exact(f"P8.density.{factor}x.{label}.exit_code", 0, value))
            record(
                harness,
                compare_true(
                    f"P8.density.{factor}x.generator.machine_report_valid",
                    item.generator_report_valid,
                ),
            )
            record(
                harness,
                compare_true(
                    f"P8.density.{factor}x.inspector.machine_report_valid",
                    item.inspector_report_valid,
                ),
            )
            record(
                correctness,
                compare_exact(
                    f"P8.density.{factor}x.payload_records",
                    baseline.payload_records * factor,
                    item.payload_records,
                ),
            )
            record(
                correctness,
                compare_exact(
                    f"P8.density.{factor}x.protocol_events.inventory_vs_canonical",
                    item.source_protocol_events,
                    item.canonical_protocol_events,
                ),
            )
            record(correctness, compare_true(f"P8.density.{factor}x.final_audit_complete", item.final_audit_complete))
            record(correctness, compare_exact(f"P8.density.{factor}x.core_gap_count", 0, item.core_gap_count))
            record(
                correctness,
                compare_at_least(
                    f"P8.density.{factor}x.data_shards_minimum",
                    minimum_density_shards,
                    item.data_shards,
                ),
            )
            record(performance, compare_at_most(f"P8.density.{factor}x.peak_private_bytes", hard_private_bytes, item.peak_private_bytes))
            for metric, value in (
                ("source_bytes", item.source_bytes),
                ("session_bytes", item.session_bytes),
                ("temporary_bytes", item.temporary_bytes),
                ("data_shards", item.data_shards),
                ("core_build_elapsed_ns", item.core_build_elapsed_ns),
                ("peak_private_bytes_observed", item.peak_private_bytes),
            ):
                record(performance, _measurement(f"P8.density.{factor}x.{metric}", value))

        for lower, upper in ((1, 2), (2, 4)):
            record(
                performance,
                _compare_ratio_at_most(
                    f"P8.density.{upper}x_vs_{lower}x.core_build_growth",
                    maximum_numerator=maximum_growth_numerator,
                    maximum_denominator=maximum_growth_denominator,
                    actual_numerator=by_factor[upper].core_build_elapsed_ns,
                    actual_denominator=by_factor[lower].core_build_elapsed_ns,
                ),
            )
        record(
            performance,
            _compare_ratio_at_most(
                "P8.density.4x_vs_1x.peak_memory_growth",
                maximum_numerator=maximum_growth_numerator,
                maximum_denominator=maximum_growth_denominator,
                actual_numerator=by_factor[4].peak_private_bytes,
                actual_denominator=by_factor[1].peak_private_bytes,
            ),
        )

    harness_failures = [v.name for v in harness if v.status == ComparisonStatus.FAIL]
    correctness_failures = [v.name for v in correctness if v.status == ComparisonStatus.FAIL]
    performance_failures = [v.name for v in performance if v.status == ComparisonStatus.FAIL]
    evidence: Mapping[str, Any] = {
        "executed": [
            "Executed_Logical2HourDuration",
            "Executed_BoundedDensityScaling",
        ],
        "not_executed": "NotExecuted_Combined2HourProductionDensity",
        "not_executed_reason": "DiskSafetyReserveAndSessionLimit",
        "harness_failures": harness_failures,
        "correctness_failures": correctness_failures,
        "performance_failures": performance_failures,
    }
    if harness_failures:
        return PhaseOutcome(
            "P8", AcceptanceStatus.HARNESS_ERROR,
            f"synthetic scale harness failed ({len(harness_failures)} comparisons)",
            repair_direction="inspect corpus generator/converter/inspector process evidence",
            evidence=evidence,
        )
    if correctness_failures:
        return PhaseOutcome(
            "P8", AcceptanceStatus.FAIL_CORRECTNESS,
            f"synthetic scale semantics failed ({len(correctness_failures)} comparisons)",
            repair_direction="inspect 64-bit frame, cross-shard lifetimes and Final Audit evidence",
            evidence=evidence,
        )
    if performance_failures:
        return PhaseOutcome(
            "P8", AcceptanceStatus.FAIL_PERFORMANCE,
            f"synthetic scale performance failed ({len(performance_failures)} comparisons)",
            repair_direction="inspect converter time/memory growth across 1x/2x/4x",
            evidence=evidence,
        )
    return PhaseOutcome(
        "P8", AcceptanceStatus.PASS,
        "logical two-hour duration and bounded 1x/2x/4x density gates passed",
        evidence=evidence,
    )


def _allocate_run_directory(root: Path) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    for ordinal in range(1, 10_000):
        candidate = root / f"attempt-{ordinal:03d}"
        try:
            candidate.mkdir(parents=False, exist_ok=False)
        except FileExistsError:
            continue
        return candidate
    raise RuntimeError("P8 attempt directory limit exceeded")


def _directory_bytes(root: Path) -> int:
    if not root.exists():
        return 0
    if root.is_file():
        return root.stat().st_size
    total = 0
    for path in root.rglob("*"):
        if path.is_file():
            total += path.stat().st_size
    return total


def _temporary_residue_bytes(session: Path) -> int:
    total = 0
    for candidate in session.parent.glob(session.name + ".building.*"):
        total += _directory_bytes(candidate)
    if session.is_dir():
        for candidate in session.rglob("*.tmp"):
            total += _directory_bytes(candidate)
    return total


def _validated_report(
    payload: str, *, mode: str, factor: int | None = None
) -> tuple[dict[str, Any], str]:
    try:
        report = parse_corpus_report(payload)
    except (TypeError, ValueError, json.JSONDecodeError) as error:
        return {}, str(error)
    if report.get("mode") != mode:
        return {}, f"expected mode={mode!r}, got {report.get('mode')!r}"
    if factor is not None and report.get("factor") != factor:
        return {}, f"expected factor={factor}, got {report.get('factor')!r}"
    return report, ""


def _run_case(
    context, work: Path, *, mode: str, factor: int, records: int | None = None
) -> tuple[
    DurationObservation | DensityObservation, Mapping[str, Any]
]:
    case_name = mode if mode == "duration" else f"density-{factor}x"
    case = work / case_name
    case.mkdir(parents=True, exist_ok=False)
    source = case / f"{case_name}.tracy-stream"
    session = case / f"{case_name}.jn-trace-session"
    release_query = context.inputs.build_query / "Release"
    corpus = release_query / "tracy-trace-session-scale-corpus.exe"
    converter = context.inputs.build_capture / "Release" / "tracy-stream-convert.exe"

    generate_command = [
        str(corpus), "generate", "--mode", mode, "--output", str(source)
    ]
    if mode == "density":
        generate_command.extend(("--factor", str(factor)))
        if records is not None:
            generate_command.extend(("--records", str(records)))
    generate, generate_stdout, generate_stderr = context.commands.run_text(
        f"P8-{case_name}-generate",
        tuple(generate_command),
        cwd=case,
        timeout_seconds=60 * 60,
        no_progress_timeout_seconds=10 * 60,
    )
    generate_report, generate_error = _validated_report(
        generate_stdout, mode=mode, factor=factor if mode == "density" else None
    )

    converter_exit = -1
    converter_elapsed = 0
    converter_peak = 0
    if generate.exit_code == 0 and generate_report and source.is_file():
        converted = context.commands.run(
            f"P8-{case_name}-convert",
            (str(converter), "-i", str(source), "-o", str(session)),
            cwd=case,
            timeout_seconds=6 * 60 * 60,
            no_progress_timeout_seconds=max(
                10 * 60,
                float(context.profile.get("converter", {}).get(
                    "no_progress_timeout_seconds", 120
                )),
            ),
        )
        converter_exit = converted.exit_code
        converter_elapsed = converted.elapsed_ns
        converter_peak = converted.peak_private_bytes

    inspector_exit = -1
    inspector_peak = 0
    inspector_report: dict[str, Any] = {}
    inspector_error = "converter did not publish a Session"
    if converter_exit == 0 and session.is_dir() and (session / "CURRENT").is_file():
        inspected, inspect_stdout, inspect_stderr = context.commands.run_text(
            f"P8-{case_name}-inspect",
            (
                str(corpus), "inspect", "--mode", mode,
                "--factor", str(factor), "--source", str(source),
                "--session", str(session),
            ),
            cwd=case,
            timeout_seconds=2 * 60 * 60,
            no_progress_timeout_seconds=30 * 60,
        )
        inspector_exit = inspected.exit_code
        inspector_peak = inspected.peak_private_bytes
        inspector_report, inspector_error = _validated_report(
            inspect_stdout, mode=mode, factor=factor
        )
        if inspected.exit_code != 0 and not inspector_error:
            inspector_error = inspect_stderr.strip() or "inspector returned non-zero"

    peak_private = max(
        converter_peak,
        inspector_peak,
        int(inspector_report.get("peak_private_bytes", 0)),
    )
    common = {
        "generator_exit_code": generate.exit_code,
        "converter_exit_code": converter_exit,
        "inspector_exit_code": inspector_exit,
        "source_protocol_events": int(inspector_report.get("source_protocol_events", 0)),
        "canonical_protocol_events": int(inspector_report.get("canonical_protocol_events", 0)),
        "final_audit_complete": inspector_report.get("final_audit_complete") is True,
        "peak_private_bytes": peak_private,
        "generator_report_valid": bool(generate_report),
        "inspector_report_valid": bool(inspector_report),
    }
    if mode == "duration":
        observation: DurationObservation | DensityObservation = DurationObservation(
            **common,
            logical_duration_ns=int(inspector_report.get("logical_duration_ns", 0)),
            maximum_frame_id=int(inspector_report.get("maximum_frame_id", 0)),
            expected_maximum_frame_id=int(generate_report.get("maximum_frame_id", 0)),
            data_shards=int(inspector_report.get("data_shards", 0)),
            zone_cross_shard=inspector_report.get("zone_cross_shard") is True,
            job_cross_shard=inspector_report.get("job_cross_shard") is True,
            allocation_cross_shard=inspector_report.get("allocation_cross_shard") is True,
            resource_cross_shard=inspector_report.get("resource_cross_shard") is True,
            io_cross_shard=inspector_report.get("io_cross_shard") is True,
        )
    else:
        observation = DensityObservation(
            **common,
            factor=factor,
            payload_records=int(generate_report.get("payload_records", 0)),
            source_bytes=source.stat().st_size if source.is_file() else 0,
            session_bytes=int(inspector_report["session_bytes"]) if "session_bytes" in inspector_report else _directory_bytes(session),
            temporary_bytes=_temporary_residue_bytes(session),
            data_shards=int(inspector_report.get("data_shards", 0)),
            core_build_elapsed_ns=converter_elapsed,
            core_gap_count=int(inspector_report.get("core_gap_count", 0)),
        )
    evidence = {
        "case": case_name,
        "source": str(source),
        "session": str(session),
        "generator_report": generate_report,
        "generator_report_error": generate_error,
        "generator_stderr": generate_stderr.strip(),
        "inspector_report": inspector_report,
        "inspector_report_error": inspector_error,
        "observation": observation.__dict__,
    }
    atomic_write_json(case / "case-result.json", evidence)
    return observation, evidence


def run_p8(context) -> PhaseOutcome:
    work = _allocate_run_directory(context.run_directory / "work" / "P8")
    duration, duration_evidence = _run_case(
        context, work, mode="duration", factor=1
    )
    density: list[DensityObservation] = []
    density_evidence: list[Mapping[str, Any]] = []
    for factor in tuple(context.profile["synthetic"]["density_factors"]):
        factor = int(factor)
        observation, evidence = _run_case(
            context,
            work,
            mode="density",
            factor=factor,
            records=int(context.profile["synthetic"]["density_base_records"]) * factor,
        )
        if not isinstance(observation, DensityObservation):
            raise TypeError("density corpus returned a duration observation")
        density.append(observation)
        density_evidence.append(evidence)
    if not isinstance(duration, DurationObservation):
        raise TypeError("duration corpus returned a density observation")
    atomic_write_json(
        work / "synthetic-scale.json",
        {
            "duration": duration_evidence,
            "density": density_evidence,
        },
    )
    policy = context.profile["synthetic"]
    outcome = evaluate_synthetic_scale(
        duration,
        density,
        expected_duration_ns=int(policy["logical_duration_hours"]) * 60 * 60 * 1_000_000_000,
        minimum_shards=int(policy["minimum_shards"]),
        maximum_growth_numerator=int(policy["maximum_growth_ratio_numerator"]),
        maximum_growth_denominator=int(policy["maximum_growth_ratio_denominator"]),
        hard_private_bytes=int(context.profile["converter"]["hard_private_bytes"]),
        recorder=context.recorder,
        minimum_density_shards=int(policy["minimum_density_shards"]),
    )
    return PhaseOutcome(
        "P8",
        outcome.status,
        outcome.summary,
        repair_direction=outcome.repair_direction,
        evidence={**dict(outcome.evidence), "work_directory": str(work)},
    )
