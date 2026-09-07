"""Machine-local path configuration with destructive-target guards."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .io_utils import load_json


class ConfigurationError(ValueError):
    pass


@dataclass(frozen=True)
class LocalInputs:
    config_path: Path
    worktree: Path
    cmake_executable: Path
    ctest_executable: Path
    python_executable: Path
    dependency_cache: Path
    short_stream: Path
    short_trace: Path
    source_30m: Path
    existing_session_30m: Path
    fresh_session_30m: Path
    scratch_root: Path
    build_capture: Path
    build_query: Path


_FIELDS = (
    "worktree",
    "cmake_executable",
    "ctest_executable",
    "python_executable",
    "dependency_cache",
    "short_stream",
    "short_trace",
    "source_30m",
    "existing_session_30m",
    "fresh_session_30m",
    "scratch_root",
    "build_capture",
    "build_query",
)


def _resolved(value: Any, field: str) -> Path:
    if not isinstance(value, str) or not value.strip():
        raise ConfigurationError(f"{field} must be a non-empty path string")
    return Path(value).expanduser().resolve()


def _same_path(left: Path, right: Path) -> bool:
    try:
        if left.exists() and right.exists():
            return left.samefile(right)
    except OSError:
        pass
    return str(left).casefold() == str(right).casefold()


def load_local_inputs(path: Path) -> LocalInputs:
    raw = load_json(path)
    if not isinstance(raw, dict):
        raise ConfigurationError("local inputs root must be an object")
    missing = [field for field in _FIELDS if field not in raw]
    if missing:
        raise ConfigurationError(f"local inputs missing fields: {missing!r}")
    values = {field: _resolved(raw[field], field) for field in _FIELDS}

    for field in ("worktree", "existing_session_30m"):
        if not values[field].is_dir():
            raise ConfigurationError(f"{field} is not an existing directory: {values[field]}")
    for field in (
        "cmake_executable",
        "ctest_executable",
        "python_executable",
        "short_stream",
        "short_trace",
        "source_30m",
    ):
        if not values[field].is_file():
            raise ConfigurationError(f"{field} is not an existing file: {values[field]}")

    formal_inputs = (
        values["short_stream"],
        values["short_trace"],
        values["source_30m"],
        values["existing_session_30m"],
    )
    writable_targets = (
        values["fresh_session_30m"],
        values["scratch_root"],
        values["build_capture"],
        values["build_query"],
    )
    for target in writable_targets:
        for formal in formal_inputs:
            if _same_path(target, formal):
                raise ConfigurationError(
                    f"writable target aliases a formal read-only input: {target}"
                )
    for index, target in enumerate(writable_targets):
        for other in writable_targets[index + 1 :]:
            if _same_path(target, other):
                raise ConfigurationError(f"writable targets alias each other: {target}")

    return LocalInputs(config_path=Path(path).resolve(), **values)
