"""Strict readers for immutable Trace Session text manifests and audit reports."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import shlex
from typing import Any, Iterable, Mapping

from .evidence import extract_acceptance_metrics


_INTEGER = re.compile(r"-?(?:0|[1-9][0-9]*)\Z")


def parse_scalar_manifest(
    payload: str, *, stop_keys: Iterable[str] = ()
) -> dict[str, Any]:
    stop = frozenset(stop_keys)
    result: dict[str, Any] = {}
    for line_number, line in enumerate(payload.splitlines(), 1):
        if not line:
            continue
        fields = shlex.split(line, posix=True)
        if not fields:
            continue
        if fields[0] in stop:
            break
        if len(fields) != 2:
            raise ValueError(f"malformed scalar manifest at line {line_number}: {line!r}")
        key, raw_value = fields
        if key in result:
            raise ValueError(f"duplicate scalar manifest key: {key}")
        result[key] = int(raw_value, 10) if _INTEGER.fullmatch(raw_value) else raw_value
    return result


_GPU_VERIFIER_METRICS = {
    "resourceCount": "gpu.verifier.resources",
    "allocationCount": "gpu.verifier.allocations",
    "passCount": "gpu.verifier.passes",
    "rangeCount": "gpu.verifier.ranges",
    "directMemberCount": "gpu.verifier.direct_member_count",
    "inclusiveMemberCount": "gpu.verifier.inclusive_member_count",
    "resourcePassRelationCount": "gpu.verifier.resource_pass_relations",
    "forwardRelationHash": "gpu.verifier.forward_relation_hash",
    "reverseRelationHash": "gpu.verifier.reverse_relation_hash",
    "reportHash": "gpu.verifier.report_hash",
    "mismatchCount": "gpu.verifier.mismatch_count",
    "sourceCatalogRecordCount": "gpu.verifier.source_catalog_record_count",
    "sourceGpuCatalogEvents": "gpu.verifier.source_gpu_catalog_events",
    "sourcePayloadBytes": "gpu.verifier.source_payload_bytes",
    "sourceRecordHash": "gpu.verifier.source_record_hash",
    "sourceReferenceRelationCount": "gpu.verifier.source_reference_relation_count",
    "engineKnownPhysicalPeakBytes": "gpu.memory.engine_known_physical_peak_bytes",
    "engineKnownPhysicalPeakTimeNs": "gpu.memory.engine_known_physical_peak_time_ns",
}


@dataclass(frozen=True)
class GpuVerifierReport:
    metrics: Mapping[str, str]
    complete: bool
    reason: str
    raw: Mapping[str, Any]


def parse_gpu_verifier_report(payload: str) -> GpuVerifierReport:
    raw = parse_scalar_manifest(payload)
    metrics = {
        target: str(raw[source])
        for source, target in _GPU_VERIFIER_METRICS.items()
        if source in raw
    }
    return GpuVerifierReport(
        metrics=metrics,
        complete=raw.get("complete") == 1,
        reason=str(raw.get("reason", "")),
        raw=raw,
    )


@dataclass(frozen=True)
class SessionArtifacts:
    root: Path
    generation: str
    generation_root: Path
    manifest: Mapping[str, Any]
    session_index: Mapping[str, Any]
    gpu_verifier: GpuVerifierReport


def load_session_artifacts(root: Path) -> SessionArtifacts:
    root = Path(root)
    generation = (root / "CURRENT").read_text(encoding="utf-8").strip()
    if not generation:
        raise ValueError("Session CURRENT is empty")
    generation_root = root / "generations" / generation
    manifest = parse_scalar_manifest(
        (generation_root / "manifest").read_text(encoding="utf-8"),
        stop_keys={"shard"},
    )
    session_index = parse_scalar_manifest(
        (generation_root / "derived" / "session-index" / "manifest").read_text(
            encoding="utf-8"
        ),
        stop_keys={"domain", "file"},
    )
    gpu_verifier = parse_gpu_verifier_report(
        (
            generation_root
            / "audit"
            / "gpu-resource-analysis-verifier"
            / "1"
            / "report"
        ).read_text(encoding="utf-8")
    )
    return SessionArtifacts(
        root=root,
        generation=generation,
        generation_root=generation_root,
        manifest=manifest,
        session_index=session_index,
        gpu_verifier=gpu_verifier,
    )


def collect_session_metrics(
    artifacts: SessionArtifacts,
    responses: Iterable[Mapping[str, Any]],
) -> dict[str, str]:
    metrics = extract_acceptance_metrics(
        response for response in responses if response.get("ok") is True
    )
    metrics["session.canonical_shards"] = str(artifacts.manifest["shard_count"])
    metrics["session.indexed_records"] = str(artifacts.session_index["indexed_records"])
    metrics.update(artifacts.gpu_verifier.metrics)
    return metrics
