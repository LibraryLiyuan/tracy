"""Golden and source identity binding."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping

from .comparators import parse_exact_int
from .io_utils import load_json, sha256_file


class GoldenBindingError(ValueError):
    pass


@dataclass(frozen=True)
class FileIdentity:
    path: str
    size: int
    sha256: str

    def as_dict(self) -> dict[str, Any]:
        return {"path": self.path, "size": str(self.size), "sha256": self.sha256}


@dataclass(frozen=True)
class Golden:
    path: str
    golden_schema: int
    query_schema: str
    session_schema: int
    source: Mapping[str, Any]
    metrics: Mapping[str, Any]
    raw: Mapping[str, Any]


def _canonical_sha256(value: Any, field: str) -> str:
    if not isinstance(value, str) or len(value) != 64:
        raise GoldenBindingError(f"{field} must be a 64-character SHA-256")
    try:
        int(value, 16)
    except ValueError as error:
        raise GoldenBindingError(f"{field} is not hexadecimal") from error
    return value.lower()


def verify_file_identity(
    path: Path, expected_size: Any, expected_sha256: str
) -> FileIdentity:
    path = Path(path)
    if not path.is_file():
        raise GoldenBindingError(f"identity target is not a file: {path}")
    expected_size_int = parse_exact_int(expected_size)
    actual_size = path.stat().st_size
    if actual_size != expected_size_int:
        raise GoldenBindingError(
            f"source size mismatch: expected={expected_size_int} actual={actual_size}"
        )
    expected_digest = _canonical_sha256(expected_sha256, "expected_sha256")
    actual_digest = sha256_file(path)
    if actual_digest != expected_digest:
        raise GoldenBindingError(
            f"source SHA-256 mismatch: expected={expected_digest} actual={actual_digest}"
        )
    return FileIdentity(str(path.resolve()), actual_size, actual_digest)


def load_golden(
    path: Path,
    *,
    expected_query_schema: str,
    expected_session_schema: int,
    expected_source_sha256: str,
) -> Golden:
    raw = load_json(path)
    if not isinstance(raw, dict):
        raise GoldenBindingError("golden root must be an object")
    if raw.get("golden_schema") != 1:
        raise GoldenBindingError("unsupported golden_schema")
    if raw.get("query_schema") != expected_query_schema:
        raise GoldenBindingError(
            f"query schema mismatch: expected={expected_query_schema!r} "
            f"actual={raw.get('query_schema')!r}"
        )
    if raw.get("session_schema") != expected_session_schema:
        raise GoldenBindingError(
            f"session schema mismatch: expected={expected_session_schema!r} "
            f"actual={raw.get('session_schema')!r}"
        )
    source = raw.get("source")
    metrics = raw.get("metrics")
    if not isinstance(source, dict) or not isinstance(metrics, dict):
        raise GoldenBindingError("golden source and metrics must be objects")
    actual_source_sha = _canonical_sha256(source.get("sha256"), "source.sha256")
    expected_source_sha = _canonical_sha256(
        expected_source_sha256, "expected_source_sha256"
    )
    if actual_source_sha != expected_source_sha:
        raise GoldenBindingError(
            f"golden source mismatch: expected={expected_source_sha} actual={actual_source_sha}"
        )
    parse_exact_int(source.get("size"))
    return Golden(
        path=str(Path(path).resolve()),
        golden_schema=1,
        query_schema=expected_query_schema,
        session_schema=expected_session_schema,
        source=source,
        metrics=metrics,
        raw=raw,
    )
