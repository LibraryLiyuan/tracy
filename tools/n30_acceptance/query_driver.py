"""Strict JSON/NDJSON boundary for the official tracy-query executable."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import Any, Iterable, Sequence

from .process_runner import ProcessRunner, ProcessResult


class QueryContractError(ValueError):
    pass


def maximum_ndjson_line_bytes(payload: bytes) -> int:
    return max((len(line) for line in payload.splitlines() if line.strip()), default=0)


def _valid_identifier(value: Any) -> bool:
    return not isinstance(value, bool) and isinstance(value, (str, int, float))


def load_ndjson_requests(path: Path) -> list[dict[str, Any]]:
    requests: list[dict[str, Any]] = []
    seen: set[tuple[type, Any]] = set()
    with Path(path).open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError as error:
                raise QueryContractError(
                    f"invalid request JSON at line {line_number}: {error}"
                ) from error
            if not isinstance(value, dict):
                raise QueryContractError(f"request line {line_number} is not an object")
            if value.get("protocol") != "tracy-query/1":
                raise QueryContractError(f"request line {line_number} has wrong protocol")
            identifier = value.get("id")
            if not _valid_identifier(identifier):
                raise QueryContractError(f"request line {line_number} has invalid id")
            key = (type(identifier), identifier)
            if key in seen:
                raise QueryContractError(f"duplicate request id: {identifier!r}")
            method = value.get("method")
            if not isinstance(method, str) or "." not in method:
                raise QueryContractError(f"request line {line_number} has invalid method")
            seen.add(key)
            requests.append(value)
    if not requests:
        raise QueryContractError("request batch is empty")
    return requests


def parse_ndjson_responses(
    payload: bytes,
    *,
    expected_ids: Sequence[Any],
    expected_schema: str = "1.34.0",
    max_response_bytes: int = 16 * 1024 * 1024,
) -> list[dict[str, Any]]:
    expected_keys = {(type(identifier), identifier) for identifier in expected_ids}
    if len(expected_keys) != len(expected_ids):
        raise QueryContractError("expected response ids are not unique")
    responses: dict[tuple[type, Any], dict[str, Any]] = {}
    for line_number, raw_line in enumerate(payload.splitlines(), 1):
        if not raw_line.strip():
            continue
        if len(raw_line) > max_response_bytes:
            raise QueryContractError(
                f"response line {line_number} exceeds {max_response_bytes} bytes"
            )
        try:
            value = json.loads(raw_line)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise QueryContractError(
                f"invalid response JSON at line {line_number}: {error}"
            ) from error
        if not isinstance(value, dict):
            raise QueryContractError(f"response line {line_number} is not an object")
        if value.get("protocol") != "tracy-query/1":
            raise QueryContractError(f"response line {line_number} has wrong protocol")
        if value.get("schema_version") != expected_schema:
            raise QueryContractError(
                f"response line {line_number} schema mismatch: "
                f"expected={expected_schema!r} actual={value.get('schema_version')!r}"
            )
        if not isinstance(value.get("ok"), bool):
            raise QueryContractError(f"response line {line_number} has invalid ok field")
        identifier = value.get("id")
        key = (type(identifier), identifier)
        if key not in expected_keys:
            raise QueryContractError(f"unexpected response id: {identifier!r}")
        if key in responses:
            raise QueryContractError(f"duplicate response id: {identifier!r}")
        responses[key] = value
    missing = [
        identifier
        for identifier in expected_ids
        if (type(identifier), identifier) not in responses
    ]
    if missing:
        raise QueryContractError(f"missing response ids: {missing!r}")
    return [responses[(type(identifier), identifier)] for identifier in expected_ids]


@dataclass(frozen=True)
class QueryTiming:
    ordinal: int
    elapsed_ns: int
    peak_private_bytes: int


@dataclass(frozen=True)
class QueryTimingSummary:
    attempts: tuple[QueryTiming, ...]
    median_elapsed_ns: int
    worst_elapsed_ns: int
    peak_private_bytes: int


@dataclass(frozen=True)
class QueryBatchRun:
    ordinal: int
    command: tuple[str, ...]
    responses: tuple[dict[str, Any], ...]
    timing: QueryTiming
    process_result: ProcessResult
    response_path: str


def summarize_timings(
    attempts: Iterable[QueryTiming], *, required_attempts: int = 3
) -> QueryTimingSummary:
    values = tuple(attempts)
    if len(values) != required_attempts:
        raise QueryContractError(
            f"expected exactly {required_attempts} timing attempts, got {len(values)}"
        )
    if required_attempts <= 0 or required_attempts % 2 == 0:
        raise QueryContractError("required_attempts must be a positive odd number")
    elapsed = sorted(value.elapsed_ns for value in values)
    return QueryTimingSummary(
        attempts=values,
        median_elapsed_ns=elapsed[len(elapsed) // 2],
        worst_elapsed_ns=max(elapsed),
        peak_private_bytes=max(value.peak_private_bytes for value in values),
    )


def execute_query_batch(
    *,
    query_executable: Path,
    trace_path: Path,
    request_path: Path,
    allow_root: Path,
    attempt_directory: Path,
    runner: ProcessRunner,
    ordinal: int,
    expected_schema: str = "1.34.0",
    max_response_bytes: int = 16 * 1024 * 1024,
    timeout_seconds: float = 120.0,
) -> QueryBatchRun:
    requests = load_ndjson_requests(request_path)
    expected_ids = [request["id"] for request in requests]
    attempt_directory = Path(attempt_directory)
    attempt_directory.mkdir(parents=True, exist_ok=True)
    response_path = attempt_directory / "responses.ndjson"
    command = (
        str(Path(query_executable)),
        "--trace",
        str(Path(trace_path)),
        "--batch",
        str(Path(request_path)),
        "--allow-root",
        str(Path(allow_root)),
        "--output",
        str(response_path),
    )
    process_result = runner.run(
        command,
        cwd=attempt_directory,
        stdout_path=attempt_directory / "stdout.log",
        stderr_path=attempt_directory / "stderr.log",
        metrics_path=attempt_directory / "metrics.ndjson",
        timeout_seconds=timeout_seconds,
    )
    if process_result.exit_code != 0:
        raise QueryContractError(
            f"tracy-query exited with {process_result.exit_code}; "
            f"stderr={attempt_directory / 'stderr.log'}"
        )
    if not response_path.is_file():
        raise QueryContractError("tracy-query did not publish its response file")
    responses = parse_ndjson_responses(
        response_path.read_bytes(),
        expected_ids=expected_ids,
        expected_schema=expected_schema,
        max_response_bytes=max_response_bytes,
    )
    timing = QueryTiming(
        ordinal=ordinal,
        elapsed_ns=process_result.elapsed_ns,
        peak_private_bytes=process_result.peak_private_bytes,
    )
    return QueryBatchRun(
        ordinal=ordinal,
        command=command,
        responses=tuple(responses),
        timing=timing,
        process_result=process_result,
        response_path=str(response_path),
    )
