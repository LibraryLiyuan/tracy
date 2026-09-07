"""P3 real short-Trace Full Worker versus Session differential gate."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

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
from .evidence import extract_acceptance_metrics
from .query_driver import (
    QueryContractError,
    load_ndjson_requests,
    maximum_ndjson_line_bytes,
    parse_ndjson_responses,
)
from .query_semantics import stable_response_hash
from .status import AcceptanceStatus


@dataclass(frozen=True)
class _QueryObservation:
    responses: tuple[dict[str, Any], ...]
    response_path: Path
    exit_code: int
    peak_private_bytes: int
    elapsed_ns: int
    max_response_bytes: int


def _missing_comparison(
    name: str, *, expected: Any, actual: Any, detail: str
) -> ComparisonResult:
    return ComparisonResult(
        name=name,
        status=ComparisonStatus.FAIL,
        expected=expected,
        actual=actual,
        delta=None,
        rule="required_value_present",
        detail=detail,
    )


def _error_code(response: Mapping[str, Any]) -> Any:
    error = response.get("error")
    return error.get("code") if isinstance(error, Mapping) else "<missing>"


def _metric_map(responses: Iterable[Mapping[str, Any]]) -> dict[str, str]:
    # Capability-unavailable responses are valid semantic evidence but do not
    # carry numeric acceptance metrics.
    return extract_acceptance_metrics(
        response for response in responses if response.get("ok") is True
    )


def evaluate_short_differential(
    full_worker_responses: Sequence[Mapping[str, Any]],
    session_responses: Sequence[Mapping[str, Any]],
    *,
    recorder: ComparisonRecorder,
) -> PhaseOutcome:
    """Compare two ordered official Query batches without tolerance or retries."""

    results: list[ComparisonResult] = []

    def record(result: ComparisonResult) -> None:
        results.append(recorder.record(result))

    record(
        compare_exact(
            "P3.response.count", len(full_worker_responses), len(session_responses)
        )
    )
    paired = min(len(full_worker_responses), len(session_responses))
    for ordinal in range(paired):
        worker = full_worker_responses[ordinal]
        session = session_responses[ordinal]
        worker_id = worker.get("id", "<missing>")
        session_id = session.get("id", "<missing>")
        request_name = str(worker_id)
        record(compare_equal(f"P3.response.{request_name}.id", worker_id, session_id))
        record(
            compare_equal(
                f"P3.response.{request_name}.protocol",
                worker.get("protocol", "<missing>"),
                session.get("protocol", "<missing>"),
            )
        )
        record(
            compare_equal(
                f"P3.response.{request_name}.schema",
                worker.get("schema_version", "<missing>"),
                session.get("schema_version", "<missing>"),
            )
        )
        worker_ok = worker.get("ok", "<missing>")
        session_ok = session.get("ok", "<missing>")
        record(compare_equal(f"P3.response.{request_name}.ok", worker_ok, session_ok))
        if worker_ok is False and session_ok is False:
            record(
                compare_equal(
                    f"P3.response.{request_name}.error_code",
                    _error_code(worker),
                    _error_code(session),
                )
            )
        elif worker_ok is True and session_ok is True:
            record(
                compare_equal(
                    f"P3.response.{request_name}.partial",
                    worker.get("partial", "<missing>"),
                    session.get("partial", "<missing>"),
                )
            )
            record(
                compare_exact(
                    f"P3.response.{request_name}.omitted_count",
                    worker.get("omitted_count", "0"),
                    session.get("omitted_count", "0"),
                )
            )
        record(
            compare_equal(
                f"P3.response.{request_name}.semantic_hash",
                stable_response_hash(worker),
                stable_response_hash(session),
            )
        )

    worker_metrics = _metric_map(full_worker_responses)
    session_metrics = _metric_map(session_responses)
    for key in sorted(set(worker_metrics) | set(session_metrics)):
        name = f"P3.metric.{key}"
        if key not in worker_metrics:
            record(
                _missing_comparison(
                    name,
                    expected="<present in Full Worker>",
                    actual=session_metrics[key],
                    detail="metric exists only in Session",
                )
            )
        elif key not in session_metrics:
            record(
                _missing_comparison(
                    name,
                    expected=worker_metrics[key],
                    actual="<missing>",
                    detail="metric is missing from Session",
                )
            )
        else:
            record(compare_exact(name, worker_metrics[key], session_metrics[key]))

    failures = [result.name for result in results if result.status == ComparisonStatus.FAIL]
    evidence = {
        "comparison_count": len(results),
        "failed_comparisons": failures,
        "full_worker_metric_count": len(worker_metrics),
        "session_metric_count": len(session_metrics),
    }
    if failures:
        return PhaseOutcome(
            "P3",
            AcceptanceStatus.FAIL_CORRECTNESS,
            f"short Trace differential found {len(failures)} exact semantic differences",
            repair_direction=(
                "inspect the named Full Worker versus Session Query differences; "
                "do not start the 30-minute cold conversion"
            ),
            evidence=evidence,
        )
    return PhaseOutcome(
        "P3",
        AcceptanceStatus.PASS,
        f"short Trace Full Worker and Session matched across {len(results)} comparisons",
        evidence=evidence,
    )


def _allocate_attempt_directory(root: Path) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    for ordinal in range(1, 10_000):
        candidate = root / f"attempt-{ordinal:03d}"
        try:
            candidate.mkdir(parents=False, exist_ok=False)
        except FileExistsError:
            continue
        return candidate
    raise RuntimeError("P3 attempt directory limit exceeded")


def _record_process_gate(
    *,
    recorder: ComparisonRecorder,
    label: str,
    exit_code: int,
    peak_private_bytes: int,
    hard_private_bytes: int,
) -> list[ComparisonResult]:
    return [
        recorder.record(compare_exact(f"P3.command.{label}.exit_code", 0, exit_code)),
        recorder.record(
            compare_at_most(
                f"P3.command.{label}.peak_private_bytes",
                hard_private_bytes,
                peak_private_bytes,
            )
        ),
    ]


def execute_audited_query(
    context,
    *,
    label: str,
    query_executable: Path,
    trace: Path,
    request_path: Path,
    allow_root: Path,
    attempt_directory: Path,
    timeout_seconds: float = 1800,
    no_progress_timeout_seconds: float = 900,
) -> _QueryObservation:
    requests = load_ndjson_requests(request_path)
    response_path = attempt_directory / "responses.ndjson"
    command = (
        str(query_executable),
        "--trace",
        str(trace),
        "--batch",
        str(request_path),
        "--allow-root",
        str(allow_root),
        "--output",
        str(response_path),
    )
    result = context.commands.run(
        label,
        command,
        cwd=attempt_directory,
        timeout_seconds=timeout_seconds,
        no_progress_timeout_seconds=no_progress_timeout_seconds,
    )
    responses: tuple[dict[str, Any], ...] = ()
    if result.exit_code == 0 and response_path.is_file():
        responses = tuple(
            parse_ndjson_responses(
                response_path.read_bytes(),
                expected_ids=[request["id"] for request in requests],
                expected_schema=str(context.profile["query_schema"]),
                max_response_bytes=int(
                    context.profile.get("query", {}).get(
                        "max_response_bytes", 16 * 1024 * 1024
                    )
                ),
            )
        )
    return _QueryObservation(
        responses=responses,
        response_path=response_path,
        exit_code=result.exit_code,
        peak_private_bytes=result.peak_private_bytes,
        elapsed_ns=result.elapsed_ns,
        max_response_bytes=(
            maximum_ndjson_line_bytes(response_path.read_bytes())
            if response_path.is_file()
            else 0
        ),
    )


def run_p3(context) -> PhaseOutcome:
    attempt = _allocate_attempt_directory(context.run_directory / "work" / "P3")
    full_directory = attempt / "full-worker"
    session_directory = attempt / "session"
    full_query_directory = attempt / "query-full-worker"
    session_query_directory = attempt / "query-session"
    for directory in (
        full_directory,
        session_directory,
        full_query_directory,
        session_query_directory,
    ):
        directory.mkdir(parents=True, exist_ok=False)

    capture_release = context.inputs.build_capture / "Release"
    query_release = context.inputs.build_query / "Release"
    legacy_converter = capture_release / "tracy-stream-convert-legacy.exe"
    session_converter = capture_release / "tracy-stream-convert.exe"
    query_executable = query_release / "tracy-query.exe"
    request_path = context.inputs.worktree / "query" / "tests" / "requests" / "n30a-session-acceptance.ndjson"
    full_trace = full_directory / "short-full-worker.tracy"
    session_trace = session_directory / "short.jn-trace-session"
    hard_private_bytes = int(
        context.profile.get("converter", {}).get("hard_private_bytes", 16 * 1024**3)
    )
    query_hard_private_bytes = int(
        context.profile.get("query", {}).get("hard_private_bytes", 16 * 1024**3)
    )

    command_results: list[ComparisonResult] = []
    legacy = context.commands.run(
        "P3-short-full-worker-convert",
        (
            str(legacy_converter),
            "-i",
            str(context.inputs.short_stream),
            "-o",
            str(full_trace),
            "-f",
            "--mode",
            "legacy",
            "--no-cache",
            "--progress-json",
            str(full_directory / "progress.json"),
            "--report-json",
            str(full_directory / "report.json"),
        ),
        cwd=full_directory,
        timeout_seconds=7200,
        no_progress_timeout_seconds=600,
    )
    command_results.extend(
        _record_process_gate(
            recorder=context.recorder,
            label="full_worker_convert",
            exit_code=legacy.exit_code,
            peak_private_bytes=legacy.peak_private_bytes,
            hard_private_bytes=hard_private_bytes,
        )
    )
    command_results.append(
        context.recorder.record(compare_true("P3.output.full_worker_trace.exists", full_trace.is_file()))
    )

    session = context.commands.run(
        "P3-short-session-convert",
        (
            str(session_converter),
            "-i",
            str(context.inputs.short_stream),
            "-o",
            str(session_trace),
            "--restart-generation",
        ),
        cwd=session_directory,
        timeout_seconds=7200,
        no_progress_timeout_seconds=600,
    )
    command_results.extend(
        _record_process_gate(
            recorder=context.recorder,
            label="session_convert",
            exit_code=session.exit_code,
            peak_private_bytes=session.peak_private_bytes,
            hard_private_bytes=hard_private_bytes,
        )
    )
    command_results.append(
        context.recorder.record(compare_true("P3.output.session.exists", session_trace.is_dir()))
    )

    conversion_failures = [
        result.name for result in command_results if result.status == ComparisonStatus.FAIL
    ]
    if conversion_failures:
        return PhaseOutcome(
            "P3",
            AcceptanceStatus.FAIL_CORRECTNESS,
            "one or both short Trace conversion paths failed",
            repair_direction="inspect preserved converter stdout, stderr, progress and report artifacts",
            evidence={
                "attempt_directory": str(attempt),
                "failed_comparisons": conversion_failures,
            },
        )

    query_checks: list[ComparisonResult] = []
    try:
        full_query = execute_audited_query(
            context,
            label="P3-query-full-worker",
            query_executable=query_executable,
            trace=full_trace,
            request_path=request_path,
            allow_root=context.run_directory,
            attempt_directory=full_query_directory,
        )
        session_query = execute_audited_query(
            context,
            label="P3-query-session",
            query_executable=query_executable,
            trace=session_trace,
            request_path=request_path,
            allow_root=context.run_directory,
            attempt_directory=session_query_directory,
        )
        for label, observation in (
            ("full_worker_query", full_query),
            ("session_query", session_query),
        ):
            query_checks.append(
                context.recorder.record(
                    compare_exact(f"P3.command.{label}.exit_code", 0, observation.exit_code)
                )
            )
            query_checks.append(
                context.recorder.record(
                    compare_at_most(
                        f"P3.command.{label}.peak_private_bytes",
                        query_hard_private_bytes,
                        observation.peak_private_bytes,
                    )
                )
            )
            query_checks.append(
                context.recorder.record(
                    compare_true(
                        f"P3.output.{label}.responses_complete",
                        bool(observation.responses),
                    )
                )
            )
    except QueryContractError as error:
        query_checks.append(
            context.recorder.record(
                ComparisonResult(
                    name="P3.query.contract",
                    status=ComparisonStatus.FAIL,
                    expected="valid complete Query 1.34 NDJSON batch",
                    actual=type(error).__name__,
                    delta=None,
                    rule="query_contract",
                    detail=str(error),
                )
            )
        )
        return PhaseOutcome(
            "P3",
            AcceptanceStatus.FAIL_CORRECTNESS,
            "short Trace Query contract failed",
            repair_direction="inspect the preserved Query stdout, stderr and partial response files",
            evidence={
                "attempt_directory": str(attempt),
                "failed_comparisons": [result.name for result in query_checks],
            },
        )

    query_failures = [
        result.name for result in query_checks if result.status == ComparisonStatus.FAIL
    ]
    if query_failures:
        return PhaseOutcome(
            "P3",
            AcceptanceStatus.FAIL_CORRECTNESS,
            "one or both short Trace Query paths failed",
            repair_direction="inspect Query exit status, memory and response completeness before comparing semantics",
            evidence={
                "attempt_directory": str(attempt),
                "failed_comparisons": query_failures,
            },
        )

    differential = evaluate_short_differential(
        full_query.responses, session_query.responses, recorder=context.recorder
    )
    return PhaseOutcome(
        "P3",
        differential.status,
        differential.summary,
        repair_direction=differential.repair_direction,
        evidence={
            **dict(differential.evidence),
            "attempt_directory": str(attempt),
            "full_worker_trace": str(full_trace),
            "session": str(session_trace),
            "full_worker_responses": str(full_query.response_path),
            "session_responses": str(session_query.response_path),
            "full_worker_peak_private_bytes": str(legacy.peak_private_bytes),
            "session_peak_private_bytes": str(session.peak_private_bytes),
        },
    )
