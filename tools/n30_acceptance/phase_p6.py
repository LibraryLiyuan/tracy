"""P6 deterministic Query matrix, pagination, latency and cancellation gates."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
import time
from typing import Any, Mapping, Sequence

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
from .io_utils import atomic_write_json
from .mcp_client import McpClient, McpContractError
from .phase_p3 import execute_audited_query
from .query_driver import QueryContractError, load_ndjson_requests
from .query_semantics import stable_response_hash
from .status import AcceptanceStatus


def require_tool_data(structured: Mapping[str, object]) -> Mapping[str, object]:
    if structured.get("ok") is not True:
        error = structured.get("error")
        code = error.get("code") if isinstance(error, Mapping) else "<missing>"
        raise ValueError(f"MCP Query failed: {code}")
    data = structured.get("data")
    if not isinstance(data, Mapping):
        raise ValueError("MCP Query response omitted data")
    return data


def extract_page(
    structured: Mapping[str, object], collection_name: str
) -> tuple[tuple[str, ...], str | None]:
    data = require_tool_data(structured)
    values = data.get(collection_name)
    if not isinstance(values, list):
        raise ValueError(f"MCP Query response omitted data.{collection_name}")
    refs: list[str] = []
    for ordinal, value in enumerate(values):
        if not isinstance(value, Mapping) or not isinstance(value.get("ref"), str):
            raise ValueError(f"MCP Query page item {ordinal} omitted ref")
        refs.append(value["ref"])
    page = structured.get("page")
    cursor = page.get("next_cursor") if isinstance(page, Mapping) else None
    if cursor is not None and not isinstance(cursor, str):
        raise ValueError("MCP Query page next_cursor is not a string or null")
    return tuple(refs), cursor


def open_mcp_session(client, trace: Path, *, timeout_seconds: float) -> tuple[str, int]:
    if timeout_seconds <= 0:
        raise ValueError("timeout_seconds must be positive")
    started = time.monotonic_ns()
    opened = client.call_tool(
        "tracy_trace_open", {"path": str(Path(trace))}, timeout_seconds=timeout_seconds
    )
    data = require_tool_data(opened)
    trace_id = data.get("trace_id")
    if not isinstance(trace_id, str) or not trace_id:
        raise ValueError("tracy_trace_open omitted trace_id")
    deadline = time.monotonic() + timeout_seconds
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"Session {trace_id} did not become ready")
        status = client.call_tool(
            "tracy_trace_status",
            {"trace_id": trace_id},
            timeout_seconds=remaining,
        )
        status_data = require_tool_data(status)
        state_value = status_data.get("status")
        state = state_value.get("state") if isinstance(state_value, Mapping) else None
        if state == "ready":
            return trace_id, time.monotonic_ns() - started
        if state in {"failed", "closed"}:
            raise ValueError(f"Session {trace_id} reached state {state}")
        time.sleep(min(0.025, remaining))


@dataclass(frozen=True)
class QueryAttemptObservation:
    ordinal: int
    response_hashes: Mapping[str, str]
    response_count: int
    expected_response_count: int
    max_response_bytes: int
    peak_private_bytes: int
    elapsed_ns: int
    exit_code: int


@dataclass(frozen=True)
class PaginationObservation:
    large_page_refs: tuple[str, ...]
    small_page_refs: tuple[tuple[str, ...], ...]
    cursors: tuple[str | None, ...]


@dataclass(frozen=True)
class QueryProbeObservation:
    ordinal: int
    query_elapsed_ns: int
    open_elapsed_ns: int
    response_bytes: int
    peak_private_bytes: int
    semantic_hash: str
    ok: bool
    total_elapsed_ns: int = 0


@dataclass(frozen=True)
class CancellationObservation:
    ordinal: int
    elapsed_ns: int
    state: str
    cancel_requested: bool
    peak_private_bytes: int


_PERFORMANCE_MARKERS = (
    "peak_private_bytes",
    "response_bytes",
    "elapsed_ns.median_gate",
    "elapsed_ns.worst_gate",
)


def _measurement(name: str, actual: int) -> ComparisonResult:
    return ComparisonResult(
        name=name,
        status=ComparisonStatus.PASS,
        expected="recorded",
        actual=actual,
        delta=0,
        rule="measurement_only",
    )


def _outcome(results: Sequence[ComparisonResult], *, success: str) -> PhaseOutcome:
    correctness = [
        result.name
        for result in results
        if result.status == ComparisonStatus.FAIL
        and not any(marker in result.name for marker in _PERFORMANCE_MARKERS)
    ]
    performance = [
        result.name
        for result in results
        if result.status == ComparisonStatus.FAIL
        and any(marker in result.name for marker in _PERFORMANCE_MARKERS)
    ]
    evidence = {
        "comparison_count": len(results),
        "correctness_failures": correctness,
        "performance_failures": performance,
    }
    if correctness:
        return PhaseOutcome(
            "P6",
            AcceptanceStatus.FAIL_CORRECTNESS,
            f"Query matrix found {len(correctness)} correctness differences",
            repair_direction="inspect the printed per-attempt response and semantic-hash comparisons",
            evidence=evidence,
        )
    if performance:
        return PhaseOutcome(
            "P6",
            AcceptanceStatus.FAIL_PERFORMANCE,
            f"Query matrix found {len(performance)} memory-limit failures",
            repair_direction="inspect the preserved Query process-tree metrics",
            evidence=evidence,
        )
    return PhaseOutcome("P6", AcceptanceStatus.PASS, success, evidence=evidence)


def evaluate_query_matrix(
    attempts: Sequence[QueryAttemptObservation],
    *,
    recorder: ComparisonRecorder,
    required_attempts: int,
    max_response_bytes: int,
    hard_private_bytes: int,
) -> PhaseOutcome:
    results: list[ComparisonResult] = []

    def record(result: ComparisonResult) -> None:
        results.append(recorder.record(result))

    record(compare_exact("P6.batch.attempt_count", required_attempts, len(attempts)))
    reference = attempts[0].response_hashes if attempts else {}
    for attempt in attempts:
        prefix = f"P6.batch.attempt_{attempt.ordinal}"
        record(compare_exact(f"{prefix}.exit_code", 0, attempt.exit_code))
        record(
            compare_exact(
                f"{prefix}.response_count",
                attempt.expected_response_count,
                attempt.response_count,
            )
        )
        record(
            compare_at_most(
                f"{prefix}.max_response_bytes",
                max_response_bytes,
                attempt.max_response_bytes,
            )
        )
        record(
            compare_at_most(
                f"{prefix}.peak_private_bytes",
                hard_private_bytes,
                attempt.peak_private_bytes,
            )
        )
        record(
            compare_equal(
                f"{prefix}.response_ids",
                tuple(sorted(reference)),
                tuple(sorted(attempt.response_hashes)),
            )
        )
        for identifier in sorted(set(reference) | set(attempt.response_hashes)):
            record(
                compare_equal(
                    f"{prefix}.{identifier}.semantic_hash",
                    reference.get(identifier, "<missing-reference>"),
                    attempt.response_hashes.get(identifier, "<missing-attempt>"),
                )
            )
    return _outcome(
        results,
        success=f"all {len(attempts)} Query batch attempts were complete and deterministic",
    )


def evaluate_pagination(
    observation: PaginationObservation, *, recorder: ComparisonRecorder,
    name: str = "P6.pagination",
) -> PhaseOutcome:
    results: list[ComparisonResult] = []

    def record(result: ComparisonResult) -> None:
        if name != "P6.pagination":
            from dataclasses import replace
            result = replace(result, name=result.name.replace("P6.pagination", name, 1))
        results.append(recorder.record(result))

    record(compare_exact("P6.pagination.small_page_count", 3, len(observation.small_page_refs)))
    record(compare_exact("P6.pagination.cursor_count", 3, len(observation.cursors)))
    for ordinal in range(min(2, len(observation.cursors))):
        record(
            compare_true(
                f"P6.pagination.page_{ordinal + 1}.has_next_cursor",
                observation.cursors[ordinal] is not None,
            )
        )
    merged = tuple(ref for page in observation.small_page_refs for ref in page)
    record(
        compare_equal(
            "P6.pagination.large_vs_three_small.ordered_refs",
            observation.large_page_refs,
            merged,
        )
    )
    record(
        compare_exact(
            "P6.pagination.unique_ref_count", len(merged), len(set(merged))
        )
    )
    return _outcome(results, success="three-page pagination is ordered, complete and duplicate-free")


def evaluate_probe_attempts(
    name: str,
    attempts: Sequence[QueryProbeObservation],
    *,
    recorder: ComparisonRecorder,
    required_attempts: int,
    query_limit_ns: int,
    open_limit_ns: int,
    response_limit_bytes: int,
    hard_private_bytes: int,
) -> PhaseOutcome:
    results: list[ComparisonResult] = []

    def record(result: ComparisonResult) -> None:
        results.append(recorder.record(result))

    prefix = f"P6.probe.{name}"
    record(compare_exact(f"{prefix}.attempt_count", required_attempts, len(attempts)))
    reference_hash = attempts[0].semantic_hash if attempts else "<missing>"
    for attempt in attempts:
        current = f"{prefix}.attempt_{attempt.ordinal}"
        record(compare_true(f"{current}.ok", attempt.ok))
        record(compare_equal(f"{current}.semantic_hash", reference_hash, attempt.semantic_hash))
        record(_measurement(f"{current}.query_elapsed_ns.measurement", attempt.query_elapsed_ns))
        record(_measurement(f"{current}.open_elapsed_ns.measurement", attempt.open_elapsed_ns))
        record(_measurement(f"{current}.total_elapsed_ns.measurement", attempt.total_elapsed_ns))
        record(
            compare_at_most(
                f"{current}.response_bytes", response_limit_bytes, attempt.response_bytes
            )
        )
        record(
            compare_at_most(
                f"{current}.peak_private_bytes",
                hard_private_bytes,
                attempt.peak_private_bytes,
            )
        )
    if attempts:
        query_median = sorted(value.query_elapsed_ns for value in attempts)[len(attempts) // 2]
        open_median = sorted(value.open_elapsed_ns for value in attempts)[len(attempts) // 2]
        record(
            compare_at_most(
                f"{prefix}.query_elapsed_ns.median_gate", query_limit_ns, query_median
            )
        )
        record(
            compare_at_most(
                f"{prefix}.open_elapsed_ns.median_gate", open_limit_ns, open_median
            )
        )
    return _outcome(results, success=f"{name} passed three-attempt Query determinism and latency gates")


def evaluate_cancellation(
    attempts: Sequence[CancellationObservation],
    *,
    recorder: ComparisonRecorder,
    required_attempts: int,
    worst_limit_ns: int,
    hard_private_bytes: int,
) -> PhaseOutcome:
    results: list[ComparisonResult] = []

    def record(result: ComparisonResult) -> None:
        results.append(recorder.record(result))

    record(compare_exact("P6.cancel.attempt_count", required_attempts, len(attempts)))
    for attempt in attempts:
        prefix = f"P6.cancel.attempt_{attempt.ordinal}"
        record(compare_true(f"{prefix}.cancel_requested", attempt.cancel_requested))
        record(compare_equal(f"{prefix}.state", "cancelled", attempt.state))
        record(_measurement(f"{prefix}.elapsed_ns.measurement", attempt.elapsed_ns))
        record(
            compare_at_most(
                f"{prefix}.elapsed_ns.worst_gate", worst_limit_ns, attempt.elapsed_ns
            )
        )
        record(
            compare_at_most(
                f"{prefix}.peak_private_bytes",
                hard_private_bytes,
                attempt.peak_private_bytes,
            )
        )
    return _outcome(results, success="all cancellation attempts completed cooperatively within the hard gate")


def _allocate_attempt_directory(root: Path, prefix: str) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    for ordinal in range(1, 10_000):
        candidate = root / f"{prefix}-{ordinal:03d}"
        try:
            candidate.mkdir(parents=False, exist_ok=False)
        except FileExistsError:
            continue
        return candidate
    raise RuntimeError(f"P6 attempt directory limit exceeded for {prefix}")


def _mcp_command(query_executable: Path, allow_root: Path) -> tuple[str, ...]:
    return (
        str(query_executable),
        "--mcp",
        "--indexed",
        "--allow-root",
        str(allow_root),
        "--allow-source-root",
        r"C:\workflow",
    )


def _new_mcp_client(
    query_executable: Path, allow_root: Path, directory: Path
) -> McpClient:
    return McpClient(
        _mcp_command(query_executable, allow_root),
        cwd=directory,
        raw_log_path=directory / "mcp.ndjson",
        stderr_path=directory / "stderr.log",
        metrics_path=directory / "metrics.ndjson",
        sample_interval_seconds=0.1,
    )


def _initialize_mcp(client: McpClient) -> None:
    response = client.request(
        "initialize",
        {
            "protocolVersion": "2025-11-25",
            "capabilities": {},
            "clientInfo": {"name": "n30a-acceptance", "version": "1"},
        },
        timeout_seconds=30,
    )
    result = response.get("result")
    if not isinstance(result, Mapping) or result.get("protocolVersion") != "2025-11-25":
        raise McpContractError("MCP initialize protocol mismatch")
    client.notify("notifications/initialized")


def _inspect(
    client: McpClient,
    trace_id: str,
    method: str,
    params: Mapping[str, Any],
    *,
    timeout_seconds: float,
) -> dict[str, Any]:
    return client.call_tool(
        "tracy_inspect",
        {"trace_id": trace_id, "method": method, "params": dict(params)},
        timeout_seconds=timeout_seconds,
    )


def _close_trace(client: McpClient, trace_id: str) -> None:
    try:
        client.call_tool(
            "tracy_trace_close", {"trace_id": trace_id}, timeout_seconds=30
        )
    except McpContractError:
        # The primary evidence has already been written.  Process shutdown still
        # releases the read-only Session mapping; the caller will preserve stderr.
        pass


def _run_probe(
    *,
    query_executable: Path,
    allow_root: Path,
    session: Path,
    directory: Path,
    ordinal: int,
    method: str,
    params: Mapping[str, Any],
    query_timeout_seconds: float,
) -> QueryProbeObservation:
    total_started = time.monotonic_ns()
    client = _new_mcp_client(query_executable, allow_root, directory)
    structured: dict[str, Any] = {}
    open_elapsed_ns = 0
    query_elapsed_ns = 0
    with client:
        _initialize_mcp(client)
        trace_id, open_elapsed_ns = open_mcp_session(
            client, session, timeout_seconds=120
        )
        started = time.monotonic_ns()
        structured = _inspect(
            client,
            trace_id,
            method,
            params,
            timeout_seconds=query_timeout_seconds,
        )
        query_elapsed_ns = time.monotonic_ns() - started
        _close_trace(client, trace_id)
    encoded = json.dumps(
        structured, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return QueryProbeObservation(
        ordinal=ordinal,
        query_elapsed_ns=query_elapsed_ns,
        open_elapsed_ns=open_elapsed_ns,
        response_bytes=len(encoded),
        peak_private_bytes=client.peak_private_bytes,
        semantic_hash=stable_response_hash(structured),
        ok=structured.get("ok") is True,
        total_elapsed_ns=time.monotonic_ns() - total_started,
    )


def _run_pagination(
    *,
    query_executable: Path,
    allow_root: Path,
    session: Path,
    directory: Path,
    method: str = "frame_image.list",
    collection: str = "images",
) -> tuple[PaginationObservation, int, int]:
    client = _new_mcp_client(query_executable, allow_root, directory)
    response_bytes = 0
    large_refs: tuple[str, ...] = ()
    small_pages: list[tuple[str, ...]] = []
    cursors: list[str | None] = []
    with client:
        _initialize_mcp(client)
        trace_id, _ = open_mcp_session(client, session, timeout_seconds=120)
        large = _inspect(
            client, trace_id, method, {"limit": 9}, timeout_seconds=30
        )
        response_bytes = max(
            response_bytes,
            len(json.dumps(large, separators=(",", ":")).encode("utf-8")),
        )
        large_refs, _ = extract_page(large, collection)
        cursor: str | None = None
        for ordinal in range(3):
            params: dict[str, Any] = {"limit": 3}
            if cursor is not None:
                params["cursor"] = cursor
            elif ordinal > 0:
                break
            page = _inspect(
                client, trace_id, method, params, timeout_seconds=30
            )
            response_bytes = max(
                response_bytes,
                len(json.dumps(page, separators=(",", ":")).encode("utf-8")),
            )
            refs, cursor = extract_page(page, collection)
            small_pages.append(refs)
            cursors.append(cursor)
        _close_trace(client, trace_id)
    return (
        PaginationObservation(large_refs, tuple(small_pages), tuple(cursors)),
        client.peak_private_bytes,
        response_bytes,
    )


def _run_cancellation(
    *,
    query_executable: Path,
    allow_root: Path,
    session: Path,
    directory: Path,
    ordinal: int,
    limit_ns: int,
) -> CancellationObservation:
    client = _new_mcp_client(query_executable, allow_root, directory)
    elapsed_ns = 0
    state = "<not-started>"
    cancel_requested = False
    with client:
        _initialize_mcp(client)
        trace_id, _ = open_mcp_session(client, session, timeout_seconds=120)
        started = time.monotonic_ns()
        submitted = client.call_tool(
            "tracy_validate",
            {
                "trace_id": trace_id,
                "async": True,
                "max_scan_events": 100_000_000,
                "max_cpu_ms": 60_000,
            },
            timeout_seconds=30,
        )
        job_id = require_tool_data(submitted).get("job_id")
        if not isinstance(job_id, str) or not job_id:
            raise ValueError("async validation omitted job_id")
        cancelled = client.call_tool(
            "tracy_job",
            {"job_id": job_id, "operation": "cancel"},
            timeout_seconds=30,
        )
        cancel_requested = require_tool_data(cancelled).get("cancel_requested") is True
        deadline = time.monotonic_ns() + limit_ns + 250_000_000
        while True:
            status = client.call_tool(
                "tracy_job",
                {"job_id": job_id, "operation": "status"},
                timeout_seconds=30,
            )
            status_data = require_tool_data(status)
            state = str(status_data.get("state", "<missing>"))
            if status_data.get("done") is True or time.monotonic_ns() >= deadline:
                break
            time.sleep(0.02)
        elapsed_ns = time.monotonic_ns() - started
        _close_trace(client, trace_id)
    return CancellationObservation(
        ordinal=ordinal,
        elapsed_ns=elapsed_ns,
        state=state,
        cancel_requested=cancel_requested,
        peak_private_bytes=client.peak_private_bytes,
    )


def _failed_contract(
    recorder: ComparisonRecorder, name: str, error: BaseException
) -> PhaseOutcome:
    timed_out = isinstance(error, TimeoutError)
    result = recorder.record(
        ComparisonResult(
            name=name,
            status=ComparisonStatus.FAIL,
            expected="response before configured timeout" if timed_out else "valid complete Query/MCP result",
            actual=type(error).__name__,
            delta="deadline exceeded; no data verdict" if timed_out else "contract failed",
            rule="query_timeout" if timed_out else "query_contract",
            detail=str(error),
        )
    )
    return PhaseOutcome(
        "P6",
        AcceptanceStatus.FAIL_PERFORMANCE if timed_out else AcceptanceStatus.FAIL_CORRECTNESS,
        f"Query/MCP {'timeout' if timed_out else 'contract failure'} at {name}",
        repair_direction="inspect the preserved MCP/Query logs; do not treat missing output as empty data",
        evidence={"failed_comparisons": [result.name]},
    )


def _aggregate_outcomes(outcomes: Sequence[PhaseOutcome], work: Path) -> PhaseOutcome:
    correctness = [value for value in outcomes if value.status == AcceptanceStatus.FAIL_CORRECTNESS]
    integrity = [value for value in outcomes if value.status == AcceptanceStatus.FAIL_INTEGRITY]
    performance = [value for value in outcomes if value.status == AcceptanceStatus.FAIL_PERFORMANCE]
    evidence = {
        "work_directory": str(work),
        "subdomains": [value.as_dict() for value in outcomes],
    }
    if correctness:
        return PhaseOutcome(
            "P6", AcceptanceStatus.FAIL_CORRECTNESS,
            f"P6 failed {len(correctness)} Query correctness subdomains",
            repair_direction="inspect the printed P6 comparisons and per-attempt raw MCP/Query evidence",
            evidence=evidence,
        )
    if integrity:
        return PhaseOutcome(
            "P6", AcceptanceStatus.FAIL_INTEGRITY,
            f"P6 failed {len(integrity)} Query integrity subdomains",
            repair_direction="inspect the preserved response contract and process evidence",
            evidence=evidence,
        )
    if performance:
        return PhaseOutcome(
            "P6", AcceptanceStatus.FAIL_PERFORMANCE,
            f"P6 failed {len(performance)} Query performance subdomains",
            repair_direction="inspect per-attempt elapsed time and process-tree memory logs",
            evidence=evidence,
        )
    return PhaseOutcome(
        "P6", AcceptanceStatus.PASS,
        "Query full-domain, determinism, pagination, latency and cancellation gates passed",
        evidence=evidence,
    )


def run_p6(context) -> PhaseOutcome:
    work = _allocate_attempt_directory(context.run_directory / "work" / "P6", "matrix")
    query_executable = context.inputs.build_query / "Release" / "tracy-query.exe"
    request_path = context.inputs.worktree / "query" / "tests" / "requests" / "n30a-session-acceptance.ndjson"
    allow_root = context.inputs.scratch_root.parent
    session = context.inputs.fresh_session_30m
    query_profile = context.profile.get("query", {})
    required_attempts = int(query_profile.get("attempts", 3))
    response_limit = int(query_profile.get("max_response_bytes", 16 * 1024 * 1024))
    hard_private = int(query_profile.get("hard_private_bytes", 16 * 1024**3))
    request_count = len(load_ndjson_requests(request_path))
    outcomes: list[PhaseOutcome] = []

    batch_attempts: list[QueryAttemptObservation] = []
    try:
        for ordinal in range(1, required_attempts + 1):
            directory = _allocate_attempt_directory(work / "batch", "attempt")
            observation = execute_audited_query(
                context,
                label=f"P6-query-batch-{ordinal}",
                query_executable=query_executable,
                trace=session,
                request_path=request_path,
                allow_root=allow_root,
                attempt_directory=directory,
                timeout_seconds=6 * 60 * 60,
                no_progress_timeout_seconds=60 * 60,
            )
            batch_attempts.append(
                QueryAttemptObservation(
                    ordinal=ordinal,
                    response_hashes={
                        str(response.get("id", "<missing>")): stable_response_hash(response)
                        for response in observation.responses
                    },
                    response_count=len(observation.responses),
                    expected_response_count=request_count,
                    max_response_bytes=observation.max_response_bytes,
                    peak_private_bytes=observation.peak_private_bytes,
                    elapsed_ns=observation.elapsed_ns,
                    exit_code=observation.exit_code,
                )
            )
        outcomes.append(
            evaluate_query_matrix(
                batch_attempts,
                recorder=context.recorder,
                required_attempts=required_attempts,
                max_response_bytes=response_limit,
                hard_private_bytes=hard_private,
            )
        )
    except (QueryContractError, ValueError) as error:
        outcomes.append(_failed_contract(context.recorder, "P6.batch.contract", error))

    probes = (
        (
            "overview",
            "trace.overview",
            {},
            int(query_profile.get("session_open_limit_ns", 2_000_000_000)),
        ),
        (
            "gpu_resource",
            "gpu.resource.search",
            {"limit": 1},
            int(query_profile.get("single_frame_resource_limit_ns", 2_000_000_000)),
        ),
        (
            "cross_domain",
            "timeline.slice",
            {
                "start_ns": "0",
                "end_ns": "9223372036854775807",
                "limit": 10,
                "tracks": ["cpu_zones", "gpu_zones", "frames", "context_switches"],
            },
            int(query_profile.get("cross_domain_limit_ns", 5_000_000_000)),
        ),
    )
    for name, method, params, query_limit_ns in probes:
        observations: list[QueryProbeObservation] = []
        try:
            for ordinal in range(1, required_attempts + 1):
                directory = _allocate_attempt_directory(work / f"probe-{name}", "attempt")
                observations.append(
                    _run_probe(
                        query_executable=query_executable,
                        allow_root=allow_root,
                        session=session,
                        directory=directory,
                        ordinal=ordinal,
                        method=method,
                        params=params,
                        query_timeout_seconds=max(30, query_limit_ns / 1_000_000_000 * 4),
                    )
                )
            outcomes.append(
                evaluate_probe_attempts(
                    name,
                    observations,
                    recorder=context.recorder,
                    required_attempts=required_attempts,
                    query_limit_ns=query_limit_ns,
                    open_limit_ns=int(query_profile.get("session_open_limit_ns", 2_000_000_000)),
                    response_limit_bytes=response_limit,
                    hard_private_bytes=hard_private,
                )
            )
        except (McpContractError, ValueError, TimeoutError) as error:
            outcomes.append(_failed_contract(context.recorder, f"P6.probe.{name}.contract", error))

    for method, collection in (("frame_image.list", "images"), ("gpu.resource.search", "resources"),
                               ("frame.list", "frames"), ("job.search", "jobs"),
                               ("zone.cpu.search", "zones"), ("sample.list", "samples")):
        try:
            pagination_directory = _allocate_attempt_directory(work / "pagination" / method, "attempt")
            pagination, pagination_memory, pagination_response = _run_pagination(
                query_executable=query_executable, allow_root=allow_root, session=session,
                directory=pagination_directory, method=method, collection=collection,
            )
            prefix = f"P6.pagination.{method}"
            outcomes.append(evaluate_pagination(pagination, recorder=context.recorder, name=prefix))
            memory_result = context.recorder.record(
                compare_at_most(f"{prefix}.peak_private_bytes", hard_private, pagination_memory))
            response_result = context.recorder.record(
                compare_at_most(f"{prefix}.response_bytes", response_limit, pagination_response))
            outcomes.append(_outcome([memory_result, response_result],
                success="pagination response and process memory stayed within limits"))
        except (McpContractError, ValueError, TimeoutError) as error:
            outcomes.append(_failed_contract(context.recorder, f"P6.pagination.{method}.contract", error))

    try:
        from .query_anchors import run_anchor_matrix
        outcomes.append(run_anchor_matrix(context, work / "anchors"))
    except (McpContractError, ValueError, TimeoutError) as error:
        outcomes.append(_failed_contract(context.recorder, "P6.anchors.contract", error))

    cancellations: list[CancellationObservation] = []
    cancel_limit = int(query_profile.get("cancel_worst_limit_ns", 2_000_000_000))
    try:
        for ordinal in range(1, required_attempts + 1):
            directory = _allocate_attempt_directory(work / "cancel", "attempt")
            cancellations.append(
                _run_cancellation(
                    query_executable=query_executable,
                    allow_root=allow_root,
                    session=session,
                    directory=directory,
                    ordinal=ordinal,
                    limit_ns=cancel_limit,
                )
            )
        outcomes.append(
            evaluate_cancellation(
                cancellations,
                recorder=context.recorder,
                required_attempts=required_attempts,
                worst_limit_ns=cancel_limit,
                hard_private_bytes=hard_private,
            )
        )
    except (McpContractError, ValueError, TimeoutError) as error:
        outcomes.append(_failed_contract(context.recorder, "P6.cancel.contract", error))

    atomic_write_json(
        work / "query-matrix.json",
        {
            "batch_attempts": [value.__dict__ for value in batch_attempts],
            "cancellation_attempts": [value.__dict__ for value in cancellations],
            "subdomains": [value.as_dict() for value in outcomes],
        },
    )
    return _aggregate_outcomes(outcomes, work)
