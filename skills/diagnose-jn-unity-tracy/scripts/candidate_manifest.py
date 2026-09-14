"""Bounded reads of saved Query candidates; no Trace access or statistics.

Large manifests reference an immutable, SHA-bound NDJSON export. Small inline
manifests remain supported. Callers must exhaust CandidateRecords to validate
the trailing checksum/count; a failed validation must not publish a report.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
from pathlib import Path

from report_common import ValidationFailure, load_json

MAX_JSON_BYTES = 16 * 1024 * 1024


class CandidateRecords:
    def __init__(self, path: Path, digest: str, count: int):
        self.path, self.digest, self.count = path, digest, count

    def __iter__(self):
        digest = hashlib.sha256()
        count = 0
        try:
            with self.path.open("rb") as source:
                while raw := source.readline(MAX_JSON_BYTES + 1):
                    if len(raw) > MAX_JSON_BYTES:
                        raise ValidationFailure(["candidate record exceeds 16 MiB; request a bounded MCP result"])
                    digest.update(raw)
                    if not raw.strip():
                        raise ValidationFailure(["candidate NDJSON contains an empty record"])
                    row = json.loads(raw)
                    if not isinstance(row, dict):
                        raise ValidationFailure(["candidate NDJSON record must be an object"])
                    count += 1
                    if count > self.count:
                        raise ValidationFailure(["candidate record count exceeds manifest"])
                    yield row
            if count != self.count or digest.hexdigest() != self.digest:
                raise ValidationFailure(["candidate record count or SHA-256 mismatch"])
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise ValidationFailure([f"cannot read candidate records: {exc}"]) from exc


def is_candidate_collection(value):
    return isinstance(value, (list, CandidateRecords))


def load_candidate_manifest(path: Path):
    try:
        path = Path(path).resolve()
        size = path.stat().st_size
    except OSError as exc:
        raise ValidationFailure([f"cannot read candidate manifest: {exc}"]) from exc
    if size > MAX_JSON_BYTES:
        raise ValidationFailure(["large inline Candidate Manifest requires SHA-bound candidate_records NDJSON"])
    manifest = load_json(path)
    if not isinstance(manifest, dict):
        raise ValidationFailure(["Candidate Manifest must be an object"])
    if "candidate_records" not in manifest:
        return manifest
    if "candidates" in manifest:
        raise ValidationFailure(["Candidate Manifest cannot mix inline candidates and candidate_records"])
    records = manifest["candidate_records"]
    if not isinstance(records, dict) or records.get("encoding") != "ndjson-v1":
        raise ValidationFailure(["unsupported candidate_records encoding"])
    relative, digest, count = records.get("path"), records.get("sha256"), records.get("count")
    if (not isinstance(relative, str) or not relative or Path(relative).is_absolute()
            or ".." in Path(relative).parts or ":" in relative):
        raise ValidationFailure(["candidate_records.path must be a safe relative path"])
    source = (path.parent / relative).resolve()
    if not source.is_relative_to(path.parent) or source == path or not source.is_file():
        raise ValidationFailure(["candidate_records.path escapes manifest root or is missing"])
    # A dedicated flat filename cannot collide with generated Markdown/JSON,
    # visuals, manifests, or the evidence-data directory.
    if relative not in {"candidates.ndjson", "candidate-records.ndjson"}:
        raise ValidationFailure(["candidate_records.path must be candidates.ndjson or candidate-records.ndjson"])
    if (not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest)
            or type(count) is not int or count < 0):
        raise ValidationFailure(["candidate_records requires SHA-256 and a nonnegative integer count"])
    manifest["candidates"] = CandidateRecords(source, digest.lower(), count)
    return manifest


def _copy_verified(source: Path, destination: Path, expected=None):
    if source.resolve() == destination.resolve():
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + f".tmp.{os.getpid()}")
    digest = hashlib.sha256()
    created = False
    try:
        with source.open("rb") as inp, temporary.open("xb") as out:
            created = True
            for chunk in iter(lambda: inp.read(1024 * 1024), b""):
                digest.update(chunk)
                out.write(chunk)
            out.flush()
            os.fsync(out.fileno())
        if expected is not None and digest.hexdigest() != expected:
            raise ValidationFailure(["candidate payload changed before report copy"])
        os.replace(temporary, destination)
    finally:
        if created:
            temporary.unlink(missing_ok=True)


def copy_candidate_manifest(path: Path, output: Path):
    """Copy exact input bytes and referenced export, preserving manifest SHA."""
    manifest = load_candidate_manifest(path)
    copied = []
    records = manifest.get("candidates")
    if isinstance(records, CandidateRecords):
        destination = output / manifest["candidate_records"]["path"]
        if not destination.resolve().is_relative_to(output.resolve()):
            raise ValidationFailure(["candidate output path escapes report root"])
        _copy_verified(records.path, destination, records.digest)
        copied.append(destination)
    destination = output / "candidate-manifest.json"
    _copy_verified(path, destination)
    return [*copied, destination]
