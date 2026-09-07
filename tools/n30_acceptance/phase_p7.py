"""P7 deterministic cancellation, resume, corruption and lease recovery gates."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
from typing import Any, Mapping, Sequence

from .acceptance_runner import PhaseOutcome
from .comparison_log import ComparisonRecorder
from .comparators import (
    ComparisonResult,
    ComparisonStatus,
    compare_at_least,
    compare_exact,
    compare_true,
)
from .io_utils import atomic_write_json, sha256_file
from .phase_p3 import execute_audited_query
from .process_runner import ConsoleBreakPlan
from .query_driver import load_ndjson_requests, parse_ndjson_responses
from .query_semantics import stable_response_hash
from .session_artifacts import load_session_artifacts, parse_scalar_manifest
from .status import AcceptanceStatus


_CPP_SUMMARY = re.compile(
    r"^N30A_CHECK_SUMMARY checks=(\d+) passed=(\d+) failed=(\d+)$", re.MULTILINE
)


def parse_cpp_test_summary(output: str) -> tuple[int, int, int]:
    match = _CPP_SUMMARY.search(output)
    if match is None:
        raise ValueError("N30A C++ check summary is missing")
    return tuple(int(value) for value in match.groups())


def find_single_building(output: Path) -> Path:
    output = Path(output)
    candidates = sorted(output.parent.glob(output.name + ".building.*"))
    if len(candidates) != 1:
        raise ValueError(
            f"expected exactly one building generation for {output}, got {len(candidates)}"
        )
    if not candidates[0].is_dir():
        raise ValueError(f"building generation is not a directory: {candidates[0]}")
    return candidates[0]


@dataclass(frozen=True)
class RecoveryObservation:
    cpp_exit_code: int
    cpp_checks: int
    cpp_passed: int
    cpp_failed: int
    single_cancel_exit_code: int
    single_break_count: int
    single_state: int
    current_absent_after_single: bool
    double_cancel_exit_code: int
    double_break_count: int
    double_state: int
    current_absent_after_double: bool
    resume_exit_code: int
    published: bool
    publishable_state: bool
    source_sha_unchanged: bool
    response_hashes_equal: bool


def evaluate_recovery(
    observation: RecoveryObservation, *, recorder: ComparisonRecorder
) -> PhaseOutcome:
    correctness: list[ComparisonResult] = []
    integrity: list[ComparisonResult] = []

    def record(target: list[ComparisonResult], result: ComparisonResult) -> None:
        target.append(recorder.record(result))

    record(correctness, compare_exact("P7.cpp.exit_code", 0, observation.cpp_exit_code))
    record(correctness, compare_at_least("P7.cpp.check_count", 400, observation.cpp_checks))
    record(correctness, compare_exact("P7.cpp.failed", 0, observation.cpp_failed))
    record(
        correctness,
        compare_exact("P7.cpp.passed_vs_checks", observation.cpp_checks, observation.cpp_passed),
    )
    record(integrity, compare_exact("P7.single_cancel.exit_code", 130, observation.single_cancel_exit_code))
    record(integrity, compare_exact("P7.single_cancel.break_count", 1, observation.single_break_count))
    record(integrity, compare_exact("P7.single_cancel.state", 9, observation.single_state))
    record(integrity, compare_true("P7.single_cancel.current_absent", observation.current_absent_after_single))
    record(integrity, compare_exact("P7.double_cancel.exit_code", 130, observation.double_cancel_exit_code))
    record(integrity, compare_exact("P7.double_cancel.break_count", 2, observation.double_break_count))
    record(integrity, compare_exact("P7.double_cancel.state", 9, observation.double_state))
    record(integrity, compare_true("P7.double_cancel.current_absent", observation.current_absent_after_double))
    record(integrity, compare_exact("P7.resume.exit_code", 0, observation.resume_exit_code))
    record(integrity, compare_true("P7.resume.published", observation.published))
    record(integrity, compare_true("P7.resume.publishable_state", observation.publishable_state))
    record(integrity, compare_true("P7.source.sha256_unchanged", observation.source_sha_unchanged))
    record(correctness, compare_true("P7.resume.response_hashes_equal", observation.response_hashes_equal))

    integrity_failures = [value.name for value in integrity if value.status == ComparisonStatus.FAIL]
    correctness_failures = [value.name for value in correctness if value.status == ComparisonStatus.FAIL]
    evidence = {
        "cpp_checks": observation.cpp_checks,
        "integrity_failures": integrity_failures,
        "correctness_failures": correctness_failures,
    }
    if integrity_failures:
        return PhaseOutcome(
            "P7", AcceptanceStatus.FAIL_INTEGRITY,
            f"recovery testing found {len(integrity_failures)} atomicity/integrity failures",
            repair_direction="inspect cancel state, CURRENT publication and source identity evidence",
            evidence=evidence,
        )
    if correctness_failures:
        return PhaseOutcome(
            "P7", AcceptanceStatus.FAIL_CORRECTNESS,
            f"recovery testing found {len(correctness_failures)} semantic differences",
            repair_direction="inspect C++ fault scenarios and resumed/uninterrupted Query hashes",
            evidence=evidence,
        )
    return PhaseOutcome(
        "P7", AcceptanceStatus.PASS,
        f"all {observation.cpp_checks} C++ checks and real single/double cancel-resume gates passed",
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
    raise RuntimeError("P7 attempt directory limit exceeded")


def _building_state(building: Path) -> int:
    manifest = parse_scalar_manifest(
        (Path(building) / "manifest").read_text(encoding="utf-8"),
        stop_keys={"shard"},
    )
    value = manifest.get("state")
    if not isinstance(value, int):
        raise ValueError(f"building manifest omitted numeric state: {building}")
    return value


def _p3_oracle_responses(context, request_path: Path) -> tuple[Mapping[str, Any], ...]:
    record = context.phase_store.load("P3")
    evidence = record.get("evidence") if isinstance(record, Mapping) else None
    response_path = evidence.get("session_responses") if isinstance(evidence, Mapping) else None
    if not isinstance(response_path, str):
        raise ValueError("P3 passed Session response evidence is unavailable")
    requests = load_ndjson_requests(request_path)
    return tuple(
        parse_ndjson_responses(
            Path(response_path).read_bytes(),
            expected_ids=[request["id"] for request in requests],
            expected_schema=str(context.profile["query_schema"]),
            max_response_bytes=int(context.profile["query"]["max_response_bytes"]),
        )
    )


def run_p7(context) -> PhaseOutcome:
    work = _allocate_run_directory(context.run_directory / "work" / "P7")
    release = context.inputs.build_query / "Release"
    cpp_test = release / "tracy-trace-session-inventory-tests.exe"
    converter = context.inputs.build_capture / "Release" / "tracy-stream-convert.exe"
    query = release / "tracy-query.exe"
    request_path = context.inputs.worktree / "query" / "tests" / "requests" / "n30a-session-acceptance.ndjson"
    output = work / "cancel-resume-short.jn-trace-session"
    source_sha_before = sha256_file(context.inputs.short_stream)

    cpp_result, cpp_stdout, cpp_stderr = context.commands.run_text(
        "P7-cpp-fault-recovery-suite",
        (str(cpp_test),),
        cwd=work,
        timeout_seconds=2 * 60 * 60,
        no_progress_timeout_seconds=30 * 60,
    )
    try:
        cpp_checks, cpp_passed, cpp_failed = parse_cpp_test_summary(
            cpp_stdout + "\n" + cpp_stderr
        )
    except ValueError as error:
        context.recorder.record(
            ComparisonResult(
                name="P7.cpp.machine_summary",
                status=ComparisonStatus.FAIL,
                expected="N30A_CHECK_SUMMARY",
                actual="<missing>",
                delta="summary missing",
                rule="machine_summary_present",
                detail=str(error),
            )
        )
        cpp_checks = cpp_passed = 0
        cpp_failed = 1

    base_command = (
        str(converter),
        "-i",
        str(context.inputs.short_stream),
        "-o",
        str(output),
    )
    single = context.commands.run(
        "P7-real-single-cancel",
        (*base_command, "--restart-generation"),
        cwd=work,
        timeout_seconds=2 * 60 * 60,
        no_progress_timeout_seconds=15 * 60,
        console_break_plan=ConsoleBreakPlan("[Canonical]"),
    )
    building = find_single_building(output)
    single_state = _building_state(building)
    current_absent_single = not (output / "CURRENT").exists()

    double = context.commands.run(
        "P7-real-double-cancel",
        base_command,
        cwd=work,
        timeout_seconds=2 * 60 * 60,
        no_progress_timeout_seconds=15 * 60,
        console_break_plan=ConsoleBreakPlan(
            "[Canonical]", second_break_delay_seconds=0.0
        ),
    )
    same_building = find_single_building(output)
    if same_building != building:
        raise ValueError(
            f"resume switched building generation: expected={building} actual={same_building}"
        )
    double_state = _building_state(same_building)
    current_absent_double = not (output / "CURRENT").exists()

    resumed = context.commands.run(
        "P7-real-resume-to-complete",
        base_command,
        cwd=work,
        timeout_seconds=6 * 60 * 60,
        no_progress_timeout_seconds=60 * 60,
    )
    published = output.is_dir() and (output / "CURRENT").is_file()
    publishable_state = False
    if published:
        final_artifacts = load_session_artifacts(output)
        publishable_state = final_artifacts.manifest.get("state") in {10, 11}
    source_sha_after = sha256_file(context.inputs.short_stream)

    oracle_responses = _p3_oracle_responses(context, request_path)
    query_directory = work / "query-resumed"
    query_directory.mkdir()
    resumed_query = execute_audited_query(
        context,
        label="P7-query-resumed-session",
        query_executable=query,
        trace=output,
        request_path=request_path,
        allow_root=context.run_directory,
        attempt_directory=query_directory,
        timeout_seconds=2 * 60 * 60,
        no_progress_timeout_seconds=30 * 60,
    )
    oracle_hashes = {
        str(response.get("id", "<missing>")): stable_response_hash(response)
        for response in oracle_responses
    }
    resumed_hashes = {
        str(response.get("id", "<missing>")): stable_response_hash(response)
        for response in resumed_query.responses
    }
    observation = RecoveryObservation(
        cpp_exit_code=cpp_result.exit_code,
        cpp_checks=cpp_checks,
        cpp_passed=cpp_passed,
        cpp_failed=cpp_failed,
        single_cancel_exit_code=single.exit_code,
        single_break_count=single.console_break_count,
        single_state=single_state,
        current_absent_after_single=current_absent_single,
        double_cancel_exit_code=double.exit_code,
        double_break_count=double.console_break_count,
        double_state=double_state,
        current_absent_after_double=current_absent_double,
        resume_exit_code=resumed.exit_code,
        published=published,
        publishable_state=publishable_state,
        source_sha_unchanged=source_sha_before == source_sha_after,
        response_hashes_equal=oracle_hashes == resumed_hashes,
    )
    atomic_write_json(
        work / "recovery-tests.json",
        {
            "observation": observation.__dict__,
            "source_sha256_before": source_sha_before,
            "source_sha256_after": source_sha_after,
            "building_generation": str(building),
            "published_session": str(output),
            "oracle_response_hashes": oracle_hashes,
            "resumed_response_hashes": resumed_hashes,
        },
    )
    outcome = evaluate_recovery(observation, recorder=context.recorder)
    return PhaseOutcome(
        "P7",
        outcome.status,
        outcome.summary,
        repair_direction=outcome.repair_direction,
        evidence={**dict(outcome.evidence), "work_directory": str(work)},
    )
