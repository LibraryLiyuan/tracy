#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Persist a safely parsed profile using a saved Query 1.35 MCP validation."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Any


SKILL_ROOT = Path(__file__).resolve().parents[1]
VENDOR_ROOT = SKILL_ROOT / "vendor"
if str(VENDOR_ROOT) not in sys.path:
    sys.path.insert(0, str(VENDOR_ROOT))

import yaml  # type: ignore  # vendored PyYAML 6.0.2, pure Python path only


MAX_PROFILE_BYTES = 1024 * 1024


class ProfileResolutionError(RuntimeError):
    pass


def _stable_json_bytes(value: Any) -> bytes:
    return (
        json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    ).encode("utf-8")


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _atomic_write(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp.{os.getpid()}")
    with temporary.open("wb") as output:
        output.write(payload)
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)


def _assert_string_keys(value: Any, path: str = "$") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if not isinstance(key, str):
                raise ProfileResolutionError(f"{path}: mapping keys must be strings")
            _assert_string_keys(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _assert_string_keys(child, f"{path}[{index}]")


def load_profile_yaml(path: Path) -> dict[str, Any]:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise ProfileResolutionError(f"cannot read analysis profile {path}: {exc}") from exc
    if len(raw) > MAX_PROFILE_BYTES:
        raise ProfileResolutionError("analysis profile exceeds the 1 MiB limit")
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise ProfileResolutionError("analysis profile must be UTF-8") from exc
    try:
        documents = list(yaml.safe_load_all(text))
    except yaml.YAMLError as exc:
        raise ProfileResolutionError(f"unsafe or invalid YAML: {exc}") from exc
    if len(documents) != 1 or not isinstance(documents[0], dict):
        raise ProfileResolutionError("analysis profile must contain exactly one mapping document")
    _assert_string_keys(documents[0])
    return documents[0]


def _contained_path(root: Path, candidate: Path) -> Path:
    root = root.resolve(strict=False)
    candidate = candidate.resolve(strict=False)
    try:
        candidate.relative_to(root)
    except ValueError as exc:
        raise ProfileResolutionError(f"output path escapes analysis root: {candidate}") from exc
    return candidate


def load_mcp_validation(path: Path) -> dict[str, Any]:
    """Read the saved response from the task's one persistent Query MCP."""
    try:
        response = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ProfileResolutionError(f"cannot read saved MCP profile validation {path}: {exc}") from exc
    if response.get("schema_version") != "1.35.0":
        raise ProfileResolutionError("saved MCP response must use Query schema 1.35.0")
    if not response.get("ok", False):
        error = response.get("error", {})
        raise ProfileResolutionError(
            f"Query MCP rejected analysis profile: {error.get('code', 'UNKNOWN')}: {error.get('message', '')}"
        )
    data = response.get("data")
    if not isinstance(data, dict) or data.get("valid") is not True:
        raise ProfileResolutionError("Query MCP response does not contain a valid normalized profile")
    return data


def persist_resolved_profile(
    output_directory: Path,
    source_profile: dict[str, Any],
    normalized_profile: dict[str, Any],
    profile_identity: str,
) -> dict[str, Path]:
    if len(profile_identity) != 64 or any(c not in "0123456789abcdefABCDEF" for c in profile_identity):
        raise ProfileResolutionError("profile identity must be a SHA-256 hexadecimal string")
    output_directory.mkdir(parents=True, exist_ok=True)
    source_json = _stable_json_bytes(source_profile)
    normalized_json = _stable_json_bytes(normalized_profile)
    normalized_yaml = yaml.safe_dump(
        normalized_profile,
        allow_unicode=True,
        default_flow_style=False,
        sort_keys=True,
    ).encode("utf-8")
    paths = {
        "source_json": output_directory / "source-analysis-profile.json",
        "json": output_directory / "resolved-analysis-profile.json",
        "yaml": output_directory / "resolved-analysis-profile.yaml",
        "manifest": output_directory / "analysis-profile-resolution.json",
    }
    _atomic_write(paths["source_json"], source_json)
    _atomic_write(paths["json"], normalized_json)
    _atomic_write(paths["yaml"], normalized_yaml)
    manifest = {
        "schema_version": 1,
        "profile_identity": profile_identity.lower(),
        "source_json_sha256": _sha256_bytes(source_json),
        "normalized_json_sha256": _sha256_bytes(normalized_json),
        "normalized_yaml_sha256": _sha256_bytes(normalized_yaml),
        "query_is_validation_authority": True,
    }
    _atomic_write(paths["manifest"], _stable_json_bytes(manifest))
    return paths


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--validation-response", type=Path, required=True)
    parser.add_argument("--analysis-root", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    args = parser.parse_args()
    try:
        if not args.analysis_root.is_absolute() or not args.output_directory.is_absolute():
            raise ProfileResolutionError("analysis root and output directory must be absolute paths")
        output_directory = _contained_path(args.analysis_root, args.output_directory)
        source_profile = load_profile_yaml(args.profile)
        validation = load_mcp_validation(args.validation_response)
        paths = persist_resolved_profile(
            output_directory,
            source_profile,
            validation["normalized_profile"],
            validation["profile_identity"],
        )
        print(json.dumps({key: str(value) for key, value in paths.items()}, ensure_ascii=False))
        return 0
    except ProfileResolutionError as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=False), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
