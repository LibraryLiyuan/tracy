"""P5 exact fresh/A7/Golden Session and independent GPU audit."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Mapping

from .acceptance_runner import PhaseOutcome
from .comparison_log import ComparisonRecorder
from .comparators import ComparisonResult, ComparisonStatus, compare_equal, compare_exact, compare_true
from .phase_p3 import execute_audited_query
from .phase_p8 import parse_corpus_report
from .io_utils import atomic_write_json
from .query_semantics import stable_response_hash
from .session_artifacts import collect_session_metrics, load_session_artifacts
from .status import AcceptanceStatus


def _required_metric(
    name: str, expected: Any, actual: Mapping[str, Any], key: str
) -> ComparisonResult:
    if key not in actual:
        return ComparisonResult(
            name=name, status=ComparisonStatus.FAIL, expected=expected,
            actual="<missing>", delta=None, rule="exact_integer",
            detail="required metric is absent",
        )
    try:
        return compare_exact(name, expected, actual[key])
    except (TypeError, ValueError) as error:
        return ComparisonResult(
            name=name, status=ComparisonStatus.FAIL, expected=expected,
            actual=actual[key], delta=None, rule="exact_integer",
            detail=f"metric is not an exact integer: {error}",
        )


def evaluate_real_30m_metrics(
    *,
    golden_metrics: Mapping[str, Any],
    fresh_metrics: Mapping[str, Any],
    existing_metrics: Mapping[str, Any],
    recorder: ComparisonRecorder,
) -> PhaseOutcome:
    results: list[ComparisonResult] = []
    for key in sorted(golden_metrics):
        fresh = _required_metric(
            f"P5.fresh_vs_golden.{key}", golden_metrics[key], fresh_metrics, key
        )
        existing = _required_metric(
            f"P5.existing_vs_golden.{key}", golden_metrics[key], existing_metrics, key
        )
        fresh_existing_expected = existing_metrics.get(key, golden_metrics[key])
        fresh_existing = _required_metric(
            f"P5.fresh_vs_existing.{key}", fresh_existing_expected, fresh_metrics, key
        )
        for result in (fresh, existing, fresh_existing):
            results.append(recorder.record(result))
    failures = [result.name for result in results if result.status == ComparisonStatus.FAIL]
    evidence = {
        "metric_count": len(golden_metrics),
        "comparison_count": len(results),
        "failed_comparisons": failures,
    }
    if failures:
        return PhaseOutcome(
            "P5", AcceptanceStatus.FAIL_CORRECTNESS,
            f"30-minute fresh/A7/Golden audit found {len(failures)} exact differences",
            repair_direction="inspect each printed metric delta and the preserved Query/GPU verifier evidence",
            evidence=evidence,
        )
    return PhaseOutcome(
        "P5", AcceptanceStatus.PASS,
        f"all {len(results)} fresh/A7/Golden metric comparisons matched exactly",
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
    raise RuntimeError("P5 attempt directory limit exceeded")


def run_p5(context) -> PhaseOutcome:
    if not isinstance(context.golden, Mapping):
        raise ValueError("P5 requires Golden")
    request_path = context.inputs.worktree / "query" / "tests" / "requests" / "n30a-session-acceptance.ndjson"
    query = context.inputs.build_query / "Release" / "tracy-query.exe"
    work = _allocate_run_directory(context.run_directory / "work" / "P5")
    fresh_dir = work / "query-fresh"
    existing_dir = work / "query-existing"
    fresh_dir.mkdir(exist_ok=False)
    existing_dir.mkdir(exist_ok=False)
    observations = {}
    audit_checks = []
    diagnostics_only = not (context.inputs.fresh_session_30m / "CURRENT").is_file()
    for label, trace, directory in (
        ("fresh", context.inputs.fresh_session_30m, fresh_dir),
        ("existing", context.inputs.existing_session_30m, existing_dir),
    ):
        if label == "fresh" and diagnostics_only:
            continue
        audited, audit_stdout, audit_stderr = context.commands.run_text(
            f"P5-canonical-final-audit-{label}",
            (str(context.inputs.build_query / "Release" / "tracy-trace-session-scale-corpus.exe"),
             "inspect", "--mode", "audit", "--source", str(context.inputs.source_30m),
             "--session", str(trace)),
            cwd=directory, timeout_seconds=8 * 60 * 60, no_progress_timeout_seconds=60 * 60,
        )
        audit_checks.append(context.recorder.record(compare_exact(f"P5.{label}.full_audit.exit_code", 0, audited.exit_code)))
        if audited.exit_code == 0:
            report = parse_corpus_report(audit_stdout)
            atomic_write_json(directory / "full-audit.json", report)
            audit_checks.append(context.recorder.record(compare_true(f"P5.{label}.full_audit.complete", report.get("final_audit_complete") is True)))
            audit_checks.append(context.recorder.record(compare_exact(f"P5.{label}.source_vs_canonical.protocol_events",
                report["source_protocol_events"], report["canonical_protocol_events"])))
        else:
            return PhaseOutcome("P5", AcceptanceStatus.FAIL_INTEGRITY,
                f"{label} full Canonical/Final Audit failed", repair_direction="inspect the preserved auditor stderr",
                evidence={"work_directory": str(work), "audit_error": audit_stderr[-8000:]})
        observation = execute_audited_query(
            context,
            label=f"P5-query-{label}-30m",
            query_executable=query,
            trace=trace,
            request_path=request_path,
            allow_root=Path(trace).parent,
            attempt_directory=directory,
            timeout_seconds=6 * 60 * 60,
            no_progress_timeout_seconds=60 * 60,
        )
        observations[label] = observation
        context.recorder.record(
            compare_exact(f"P5.query.{label}.exit_code", 0, observation.exit_code)
        )
        context.recorder.record(
            compare_true(f"P5.query.{label}.responses_complete", bool(observation.responses))
        )
    if diagnostics_only:
        artifacts = load_session_artifacts(context.inputs.existing_session_30m)
        metrics = collect_session_metrics(artifacts, observations["existing"].responses)
        checks = [context.recorder.record(_required_metric(f"P5.existing_vs_golden.{key}",
                  value, metrics, key)) for key, value in sorted(context.golden["metrics"].items())]
        failed = [check.name for check in checks if check.status == ComparisonStatus.FAIL]
        return PhaseOutcome("P5", AcceptanceStatus.FAIL_CORRECTNESS if failed else AcceptanceStatus.BLOCKED_ENVIRONMENT,
            "existing A7 read-only audit finished; fresh Session comparison was not executed",
            repair_direction="resolve the preceding gate before fresh conversion and three-way comparison",
            evidence={"diagnostics_only": True, "failed_comparisons": failed, "work_directory": str(work)})
    if not observations["fresh"].responses or not observations["existing"].responses:
        return PhaseOutcome(
            "P5", AcceptanceStatus.FAIL_CORRECTNESS,
            "one or both 30-minute Query batches did not return complete evidence",
            repair_direction="inspect preserved Query logs and partial NDJSON; do not infer missing metrics",
            evidence={"work_directory": str(work)},
        )

    fresh_artifacts = load_session_artifacts(context.inputs.fresh_session_30m)
    existing_artifacts = load_session_artifacts(context.inputs.existing_session_30m)
    artifact_checks = [
        compare_true("P5.fresh.gpu_verifier.complete", fresh_artifacts.gpu_verifier.complete),
        compare_equal("P5.fresh.gpu_verifier.reason", "", fresh_artifacts.gpu_verifier.reason),
        compare_true("P5.existing.gpu_verifier.complete", existing_artifacts.gpu_verifier.complete),
        compare_equal("P5.existing.gpu_verifier.reason", "", existing_artifacts.gpu_verifier.reason),
        compare_equal(
            "P5.fresh.manifest.reason", context.golden["expected_degradation_reason"],
            fresh_artifacts.manifest.get("reason", ""),
        ),
        compare_equal(
            "P5.existing.manifest.reason", context.golden["expected_degradation_reason"],
            existing_artifacts.manifest.get("reason", ""),
        ),
    ]
    for result in artifact_checks:
        context.recorder.record(result)

    fresh_responses = observations["fresh"].responses
    existing_responses = observations["existing"].responses
    semantic_checks: list[ComparisonResult] = []
    for fresh, existing in zip(fresh_responses, existing_responses):
        request_id = str(fresh.get("id", "<missing>"))
        result = compare_equal(
            f"P5.response.{request_id}.semantic_hash",
            stable_response_hash(existing), stable_response_hash(fresh),
        )
        semantic_checks.append(context.recorder.record(result))

    metric_outcome = evaluate_real_30m_metrics(
        golden_metrics=context.golden["metrics"],
        fresh_metrics=collect_session_metrics(fresh_artifacts, fresh_responses),
        existing_metrics=collect_session_metrics(existing_artifacts, existing_responses),
        recorder=context.recorder,
    )
    auxiliary_failures = [
        result.name for result in (*audit_checks, *artifact_checks, *semantic_checks)
        if result.status == ComparisonStatus.FAIL
    ]
    if auxiliary_failures:
        return PhaseOutcome(
            "P5", AcceptanceStatus.FAIL_CORRECTNESS,
            f"30-minute manifest/verifier/Query semantics found {len(auxiliary_failures)} differences",
            repair_direction="inspect the printed semantic hashes and verifier completeness before downstream tests",
            evidence={
                **dict(metric_outcome.evidence),
                "failed_comparisons": auxiliary_failures + list(metric_outcome.evidence.get("failed_comparisons", [])),
                "work_directory": str(work),
            },
        )
    return PhaseOutcome(
        "P5", metric_outcome.status, metric_outcome.summary,
        repair_direction=metric_outcome.repair_direction,
        evidence={**dict(metric_outcome.evidence), "work_directory": str(work)},
    )
