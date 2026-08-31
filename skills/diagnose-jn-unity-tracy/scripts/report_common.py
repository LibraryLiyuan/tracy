"""Shared validation and deterministic rendering helpers for JN Tracy reports."""

from __future__ import annotations

import hashlib
import html
import json
import math
import re
from pathlib import Path
from typing import Any, Iterable


ANALYSIS_SCHEMA_VERSION = 2
EVIDENCE_SCHEMA_VERSION = 1
REPORT_MANIFEST_SCHEMA_VERSION = 1
REPORT_BUILDER_VERSION = "1.1.1"
TEMPLATE_VERSION = "1.1.1"
ANALYSIS_WORKFLOW_VERSION = "1.1.1"

PRIMARY_LIMITERS = {
    "IntentionalPacing",
    "MainThreadCpuWork",
    "MainWaitsJob",
    "MainWaitsRender",
    "RenderThreadCpuWork",
    "JobWorkerCpuWork",
    "RenderWaitsGpuFencePresent",
    "GpuCriticalPath",
    "LoadingIo",
    "CpuMemoryPressure",
    "GpuMemoryPressure",
    "OsSchedulingCpuContention",
    "TracyRuntimeOverhead",
    "TracyAnalysisPressure",
    "Mixed",
    "Unknown",
}
FRAME_CLASSES = {
    "StableWithinBudget",
    "StablePressure",
    "RecurrentSpike",
    "IsolatedSpike",
    "Transition",
    "Unclassified",
}
STACK_PROVENANCE = {
    "PerEventExactNative",
    "SiteReusedNative",
    "ExactManagedSource",
    "ExactLuaSource",
    "SymbolOnly",
    "MarkerOnly",
    "Unavailable",
}
DEPTHS = {f"L{i}" for i in range(7)}
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
SAFE_ID_RE = re.compile(r"^[A-Za-z0-9_.:-]+$")


class ValidationFailure(Exception):
    def __init__(self, errors: Iterable[str]):
        self.errors = list(errors)
        super().__init__("; ".join(self.errors))


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValidationFailure([f"cannot read JSON {path}: {exc}"]) from exc


def stable_json_bytes(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(stable_json_bytes(value))


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _require_mapping(value: Any, path: str, errors: list[str]) -> dict[str, Any]:
    if not isinstance(value, dict):
        errors.append(f"{path} must be an object")
        return {}
    return value


def _require_list(value: Any, path: str, errors: list[str]) -> list[Any]:
    if not isinstance(value, list):
        errors.append(f"{path} must be an array")
        return []
    return value


def _required(obj: dict[str, Any], fields: Iterable[str], path: str, errors: list[str]) -> None:
    for field in fields:
        if field not in obj:
            errors.append(f"{path}.{field} is required")


def _validate_sha(value: Any, path: str, errors: list[str]) -> None:
    if not isinstance(value, str) or not SHA256_RE.fullmatch(value):
        errors.append(f"{path} must be a 64-character SHA-256")


def _safe_relative(root: Path, value: Any, path: str, errors: list[str]) -> Path | None:
    if not isinstance(value, str) or not value:
        errors.append(f"{path} must be a non-empty relative path")
        return None
    candidate = Path(value)
    if candidate.is_absolute() or ".." in candidate.parts:
        errors.append(f"{path} escapes the evidence root")
        return None
    resolved_root = root.resolve()
    resolved = (root / candidate).resolve()
    if resolved != resolved_root and resolved_root not in resolved.parents:
        errors.append(f"{path} escapes the evidence root")
        return None
    return resolved


def _validate_timeline(data: Any, expected_trace_id: str, expected_sha: str, path: str) -> list[str]:
    errors: list[str] = []
    obj = _require_mapping(data, path, errors)
    if obj.get("trace_id") != expected_trace_id:
        errors.append(f"{path}.trace_id does not match evidence trace")
    if obj.get("trace_sha256") != expected_sha:
        errors.append(f"{path}.trace_sha256 does not match evidence trace")
    try:
        start_ns = int(obj.get("start_ns"))
        end_ns = int(obj.get("end_ns"))
        if start_ns > end_ns:
            errors.append(f"{path} time reversal: start_ns > end_ns")
    except (TypeError, ValueError):
        errors.append(f"{path} start_ns/end_ns must be integers")
        start_ns, end_ns = 0, -1

    lane_ids: set[str] = set()
    for index, lane in enumerate(_require_list(obj.get("lanes"), f"{path}.lanes", errors)):
        lane_obj = _require_mapping(lane, f"{path}.lanes[{index}]", errors)
        lane_id = lane_obj.get("lane_id")
        if not isinstance(lane_id, str) or not lane_id:
            errors.append(f"{path}.lanes[{index}].lane_id is required")
        elif lane_id in lane_ids:
            errors.append(f"{path}.lanes[{index}].lane_id is duplicated")
        else:
            lane_ids.add(lane_id)

    zones: dict[str, dict[str, Any]] = {}
    for index, zone in enumerate(_require_list(obj.get("zones"), f"{path}.zones", errors)):
        zone_path = f"{path}.zones[{index}]"
        zone_obj = _require_mapping(zone, zone_path, errors)
        zone_ref = zone_obj.get("zone_ref")
        if not isinstance(zone_ref, str) or not zone_ref:
            errors.append(f"{zone_path}.zone_ref is required")
            continue
        if zone_ref in zones:
            errors.append(f"{zone_path}.zone_ref is duplicated")
        zones[zone_ref] = zone_obj
        if zone_obj.get("lane_id") not in lane_ids:
            errors.append(f"{zone_path}.lane_id does not reference a declared lane")
        try:
            zone_start = int(zone_obj.get("start_ns"))
            zone_end = int(zone_obj.get("end_ns"))
            if zone_start > zone_end:
                errors.append(f"{zone_path} time reversal: start_ns > end_ns")
            if end_ns >= start_ns and (zone_start < start_ns or zone_end > end_ns):
                errors.append(f"{zone_path} lies outside the evidence time range")
        except (TypeError, ValueError):
            errors.append(f"{zone_path} start_ns/end_ns must be integers")

    for zone_ref, zone in zones.items():
        parent_ref = zone.get("parent_ref")
        if parent_ref is None:
            continue
        if parent_ref not in zones:
            errors.append(f"{path}.zones[{zone_ref}] parent_ref is missing")
            continue
        parent = zones[parent_ref]
        try:
            if int(zone["start_ns"]) < int(parent["start_ns"]) or int(zone["end_ns"]) > int(parent["end_ns"]):
                errors.append(f"{path}.zones[{zone_ref}] is outside its parent interval")
        except (KeyError, TypeError, ValueError):
            pass
    relation_refs: set[str] = set()
    for index, relation in enumerate(_require_list(obj.get("relations"), f"{path}.relations", errors)):
        relation_path = f"{path}.relations[{index}]"
        relation_obj = _require_mapping(relation, relation_path, errors)
        _required(relation_obj, ["relation_ref", "from_ref", "to_ref", "kind"], relation_path, errors)
        relation_ref = relation_obj.get("relation_ref")
        if not isinstance(relation_ref, str) or not relation_ref:
            errors.append(f"{relation_path}.relation_ref is required")
        elif relation_ref in relation_refs:
            errors.append(f"{relation_path}.relation_ref is duplicated")
        else:
            relation_refs.add(relation_ref)
        if relation_obj.get("from_ref") not in zones:
            errors.append(f"{relation_path}.from_ref does not reference a timeline entity")
        if relation_obj.get("to_ref") not in zones:
            errors.append(f"{relation_path}.to_ref does not reference a timeline entity")
    return errors


def validate_analysis_and_evidence(
    analysis: Any,
    evidence: Any,
    evidence_root: Path,
) -> tuple[list[str], dict[str, dict[str, Any]], dict[str, Any]]:
    errors: list[str] = []
    analysis_obj = _require_mapping(analysis, "analysis", errors)
    evidence_obj = _require_mapping(evidence, "evidence_manifest", errors)

    if analysis_obj.get("schema_version") != ANALYSIS_SCHEMA_VERSION:
        errors.append(f"analysis.schema_version must be {ANALYSIS_SCHEMA_VERSION}")
    if evidence_obj.get("schema_version") != EVIDENCE_SCHEMA_VERSION:
        errors.append(f"evidence_manifest.schema_version must be {EVIDENCE_SCHEMA_VERSION}")

    analysis_required = [
        "report_identity", "analysis_scope", "trace_quality", "frame_class_summary",
        "bottleneck_signatures", "evidence_cards", "domain_health", "cpu_memory_findings",
        "gpu_memory_findings", "tracy_findings", "evidence_gaps", "query_audit_refs", "report_versions",
    ]
    _required(analysis_obj, analysis_required, "analysis", errors)
    identity = _require_mapping(analysis_obj.get("report_identity"), "analysis.report_identity", errors)
    _required(identity, ["report_id", "title", "generated_at", "trace_id", "trace_sha256", "configuration"], "analysis.report_identity", errors)
    _validate_sha(identity.get("trace_sha256"), "analysis.report_identity.trace_sha256", errors)
    configuration = _require_mapping(identity.get("configuration"), "analysis.report_identity.configuration", errors)
    for key in ("local_profile", "project_profile", "performance_budgets", "marker_attribution"):
        config = _require_mapping(configuration.get(key), f"analysis.report_identity.configuration.{key}", errors)
        _required(config, ["path", "sha256"], f"analysis.report_identity.configuration.{key}", errors)
        _validate_sha(config.get("sha256"), f"analysis.report_identity.configuration.{key}.sha256", errors)

    scope = _require_mapping(analysis_obj.get("analysis_scope"), "analysis.analysis_scope", errors)
    _required(scope, ["mode", "start_ns", "end_ns", "valid_frame_count", "excluded_frame_count"], "analysis.analysis_scope", errors)
    try:
        if int(scope.get("start_ns")) > int(scope.get("end_ns")):
            errors.append("analysis.analysis_scope has time reversal")
    except (TypeError, ValueError):
        errors.append("analysis.analysis_scope start_ns/end_ns must be integers")

    manifest_trace = _require_mapping(evidence_obj.get("trace"), "evidence_manifest.trace", errors)
    _required(manifest_trace, ["trace_id", "sha256"], "evidence_manifest.trace", errors)
    _validate_sha(manifest_trace.get("sha256"), "evidence_manifest.trace.sha256", errors)
    if identity.get("trace_id") != manifest_trace.get("trace_id"):
        errors.append("analysis trace_id does not match evidence manifest trace_id")
    if identity.get("trace_sha256") != manifest_trace.get("sha256"):
        errors.append("analysis trace_sha256 does not match evidence manifest trace_sha256")

    evidence_by_id: dict[str, dict[str, Any]] = {}
    data_by_id: dict[str, Any] = {}
    available_entity_refs: set[str] = set()
    available_relation_refs: set[str] = set()
    evidence_items = _require_list(evidence_obj.get("evidence"), "evidence_manifest.evidence", errors)
    for index, item in enumerate(evidence_items):
        item_path = f"evidence_manifest.evidence[{index}]"
        obj = _require_mapping(item, item_path, errors)
        _required(
            obj,
            ["evidence_id", "trace_id", "trace_sha256", "type", "mcp", "request", "response", "refs", "partial", "truncated", "next_cursor", "attempt", "quality", "data_path"],
            item_path,
            errors,
        )
        evidence_id = obj.get("evidence_id")
        if not isinstance(evidence_id, str) or not SAFE_ID_RE.fullmatch(evidence_id):
            errors.append(f"{item_path}.evidence_id is invalid")
            continue
        if evidence_id in evidence_by_id:
            errors.append(f"{item_path}.evidence_id is duplicated")
            continue
        evidence_by_id[evidence_id] = obj
        if obj.get("trace_id") != manifest_trace.get("trace_id"):
            errors.append(f"{item_path}.trace_id does not match manifest trace")
        if obj.get("trace_sha256") != manifest_trace.get("sha256"):
            errors.append(f"{item_path}.trace_sha256 does not match manifest trace_sha256")
        try:
            attempt = int(obj.get("attempt"))
            if attempt < 1 or attempt > 3:
                errors.append(f"{item_path}.attempt must be in [1,3]")
        except (TypeError, ValueError):
            errors.append(f"{item_path}.attempt must be an integer")
        if bool(obj.get("partial")) and obj.get("quality") == "complete":
            errors.append(f"{item_path} is partial but quality is complete")
        if bool(obj.get("truncated")) and obj.get("quality") not in {"truncated", "partial", "degraded"}:
            errors.append(f"{item_path} is truncated but quality does not disclose it")

        data_path = _safe_relative(evidence_root, obj.get("data_path"), f"{item_path}.data_path", errors)
        if data_path is None:
            continue
        if not data_path.is_file():
            errors.append(f"{item_path}.data_path does not exist: {obj.get('data_path')}")
            continue
        response = _require_mapping(obj.get("response"), f"{item_path}.response", errors)
        _validate_sha(response.get("sha256"), f"{item_path}.response.sha256", errors)
        if SHA256_RE.fullmatch(str(response.get("sha256", ""))) and sha256_file(data_path) != str(response.get("sha256")).lower():
            errors.append(f"{item_path}.response.sha256 does not match data_path")
        try:
            data = load_json(data_path)
            data_by_id[evidence_id] = data
            if obj.get("type") in {"correlated_timeline", "zone_timeline", "gpu_timeline"}:
                errors.extend(_validate_timeline(data, str(manifest_trace.get("trace_id")), str(manifest_trace.get("sha256")), f"evidence[{evidence_id}].data"))
                if isinstance(data, dict):
                    for zone in data.get("zones", []):
                        if isinstance(zone, dict) and isinstance(zone.get("zone_ref"), str):
                            available_entity_refs.add(zone["zone_ref"])
                    for relation in data.get("relations", []):
                        if isinstance(relation, dict) and isinstance(relation.get("relation_ref"), str):
                            available_relation_refs.add(relation["relation_ref"])
            elif obj.get("type") == "memory_timeline":
                data_obj = _require_mapping(data, f"evidence[{evidence_id}].data", errors)
                if data_obj.get("trace_id") != manifest_trace.get("trace_id") or data_obj.get("trace_sha256") != manifest_trace.get("sha256"):
                    errors.append(f"evidence[{evidence_id}].data trace identity does not match manifest")
                for series_index, series in enumerate(_require_list(data_obj.get("series"), f"evidence[{evidence_id}].data.series", errors)):
                    series_obj = _require_mapping(series, f"evidence[{evidence_id}].data.series[{series_index}]", errors)
                    previous_time: int | None = None
                    for point_index, point in enumerate(_require_list(series_obj.get("points"), f"evidence[{evidence_id}].data.series[{series_index}].points", errors)):
                        point_obj = _require_mapping(point, f"evidence[{evidence_id}].data.series[{series_index}].points[{point_index}]", errors)
                        try:
                            point_time = int(point_obj.get("time_ns"))
                            point_value = int(point_obj.get("value_bytes"))
                            if previous_time is not None and point_time < previous_time:
                                errors.append(f"evidence[{evidence_id}].data memory timeline time reversal")
                            if point_value < 0:
                                errors.append(f"evidence[{evidence_id}].data memory value is negative")
                            previous_time = point_time
                        except (TypeError, ValueError):
                            errors.append(f"evidence[{evidence_id}].data memory point must contain integer time_ns/value_bytes")
            elif obj.get("type") == "resource_relation_graph":
                data_obj = _require_mapping(data, f"evidence[{evidence_id}].data", errors)
                if data_obj.get("trace_id") != manifest_trace.get("trace_id") or data_obj.get("trace_sha256") != manifest_trace.get("sha256"):
                    errors.append(f"evidence[{evidence_id}].data trace identity does not match manifest")
                graph_nodes: set[str] = set()
                for node in _require_list(data_obj.get("nodes"), f"evidence[{evidence_id}].data.nodes", errors):
                    if isinstance(node, dict) and isinstance(node.get("ref"), str):
                        graph_nodes.add(node["ref"])
                        available_entity_refs.add(node["ref"])
                for edge_index, edge in enumerate(_require_list(data_obj.get("edges"), f"evidence[{evidence_id}].data.edges", errors)):
                    edge_obj = _require_mapping(edge, f"evidence[{evidence_id}].data.edges[{edge_index}]", errors)
                    if edge_obj.get("from_ref") not in graph_nodes or edge_obj.get("to_ref") not in graph_nodes:
                        errors.append(f"evidence[{evidence_id}].data.edges[{edge_index}] endpoint is missing")
                    if isinstance(edge_obj.get("relation_ref"), str):
                        available_relation_refs.add(edge_obj["relation_ref"])
            elif obj.get("type") == "frame_image":
                data_obj = _require_mapping(data, f"evidence[{evidence_id}].data", errors)
                if data_obj.get("trace_id") != manifest_trace.get("trace_id") or data_obj.get("trace_sha256") != manifest_trace.get("sha256"):
                    errors.append(f"evidence[{evidence_id}].data trace identity does not match manifest")
                mcp = _require_mapping(obj.get("mcp"), f"{item_path}.mcp", errors)
                mcp_params = _require_mapping(mcp.get("params"), f"{item_path}.mcp.params", errors)
                resource_uri = mcp_params.get("uri")
                if mcp.get("method") != "resources/read" or not isinstance(resource_uri, str) or not resource_uri:
                    errors.append(f"evidence[{evidence_id}] FrameImage must come from MCP resources/read")
                if data_obj.get("source") != "mcp_resources_read":
                    errors.append(f"evidence[{evidence_id}].data.source must be mcp_resources_read")
                if data_obj.get("resource_uri") != resource_uri:
                    errors.append(f"evidence[{evidence_id}].data.resource_uri must match MCP resources/read uri")
                if data_obj.get("mime_type") != "image/png":
                    errors.append(f"evidence[{evidence_id}].data.mime_type must be image/png")
                image_path = _safe_relative(evidence_root, data_obj.get("image_path"), f"evidence[{evidence_id}].data.image_path", errors)
                _validate_sha(data_obj.get("image_sha256"), f"evidence[{evidence_id}].data.image_sha256", errors)
                if image_path is not None:
                    if not image_path.is_file():
                        errors.append(f"evidence[{evidence_id}].data.image_path does not exist")
                    else:
                        if image_path.suffix.lower() != ".png" or image_path.read_bytes()[:8] != b"\x89PNG\r\n\x1a\n":
                            errors.append(f"evidence[{evidence_id}].data.image_path must be the PNG returned by MCP resources/read")
                        if SHA256_RE.fullmatch(str(data_obj.get("image_sha256", ""))) and sha256_file(image_path) != str(data_obj.get("image_sha256")).lower():
                            errors.append(f"evidence[{evidence_id}].data.image_sha256 does not match image_path")
        except ValidationFailure as exc:
            errors.extend(exc.errors)

    cards = _require_list(analysis_obj.get("evidence_cards"), "analysis.evidence_cards", errors)
    card_by_signature: dict[str, dict[str, Any]] = {}
    for index, card in enumerate(cards):
        card_obj = _require_mapping(card, f"analysis.evidence_cards[{index}]", errors)
        _required(card_obj, ["id", "signature_id", "title", "evidence_refs", "visuals"], f"analysis.evidence_cards[{index}]", errors)
        signature_id = card_obj.get("signature_id")
        if isinstance(signature_id, str):
            if signature_id in card_by_signature:
                errors.append(f"multiple Evidence Cards reference signature {signature_id}")
            card_by_signature[signature_id] = card_obj
        for evidence_id in _require_list(card_obj.get("evidence_refs"), f"analysis.evidence_cards[{index}].evidence_refs", errors):
            if evidence_id not in evidence_by_id:
                errors.append(f"analysis.evidence_cards[{index}] references missing evidence {evidence_id}")

    signatures = _require_list(analysis_obj.get("bottleneck_signatures"), "analysis.bottleneck_signatures", errors)
    signature_ids: set[str] = set()
    for index, signature in enumerate(signatures):
        path = f"analysis.bottleneck_signatures[{index}]"
        obj = _require_mapping(signature, path, errors)
        _required(
            obj,
            [
                "id", "stable_key", "significance_reasons", "affected_frames", "affected_ranges", "frequency",
                "budget_debt", "severity", "primary_limiter", "secondary_contributors", "representative_instances",
                "causal_chain", "hypothesis_ledger", "deepest_level", "evidence_refs", "stack_source_provenance",
                "supported_claims", "unsupported_claims", "optimization_direction", "functional_risk",
                "theoretical_upper_bound", "retest_metrics", "confidence", "conclusion_status", "evidence_gap",
            ],
            path,
            errors,
        )
        signature_id = obj.get("id")
        if not isinstance(signature_id, str) or not SAFE_ID_RE.fullmatch(signature_id):
            errors.append(f"{path}.id is invalid")
        elif signature_id in signature_ids:
            errors.append(f"{path}.id is duplicated")
        else:
            signature_ids.add(signature_id)
        stable_key = obj.get("stable_key")
        if not isinstance(stable_key, str) or len(stable_key.split("/")) != 3:
            errors.append(f"{path}.stable_key must be Domain/Limiter/Mechanism.StableEntity")
        if isinstance(stable_key, str):
            legacy_terms = {"Telemetry", "Memory", "RenderActiveWork", "MainThreadActiveWork", "TracyPerturbation", "GcAllocationMemory"}
            if any(part in legacy_terms for part in stable_key.split("/")):
                errors.append(f"{path}.stable_key uses ambiguous legacy terminology")
        if isinstance(stable_key, str) and re.search(r"(?i)(^|[^a-z])n\d{2}([^a-z]|$)", stable_key):
            errors.append(f"{path}.stable_key must not contain a development version")
        if obj.get("primary_limiter") not in PRIMARY_LIMITERS:
            errors.append(f"{path}.primary_limiter is invalid")
        deepest = obj.get("deepest_level")
        if deepest not in DEPTHS:
            errors.append(f"{path}.deepest_level is invalid")
        if deepest != "L6" and not isinstance(obj.get("evidence_gap"), dict):
            errors.append(f"{path} must reach L6 or record an EvidenceGap")
        if signature_id not in card_by_signature:
            errors.append(f"{path} is significant but has no Evidence Card")
        for provenance in _require_list(obj.get("stack_source_provenance"), f"{path}.stack_source_provenance", errors):
            if provenance not in STACK_PROVENANCE:
                errors.append(f"{path}.stack_source_provenance contains invalid value {provenance}")
        for evidence_id in _require_list(obj.get("evidence_refs"), f"{path}.evidence_refs", errors):
            if evidence_id not in evidence_by_id:
                errors.append(f"{path} references missing evidence {evidence_id}")
        limiter = obj.get("primary_limiter")
        for edge_index, edge in enumerate(_require_list(obj.get("causal_chain"), f"{path}.causal_chain", errors)):
            edge_obj = _require_mapping(edge, f"{path}.causal_chain[{edge_index}]", errors)
            _required(edge_obj, ["relation_ref", "from_ref", "to_ref", "relation"], f"{path}.causal_chain[{edge_index}]", errors)
            if edge_obj.get("relation_ref") not in available_relation_refs:
                errors.append(f"{path}.causal_chain[{edge_index}].relation_ref is not present in Evidence")
            if edge_obj.get("from_ref") not in available_entity_refs or edge_obj.get("to_ref") not in available_entity_refs:
                errors.append(f"{path}.causal_chain[{edge_index}] endpoint is not present in Evidence")
        if limiter in {"MainWaitsJob", "MainWaitsRender", "RenderWaitsGpuFencePresent"}:
            chain = _require_list(obj.get("causal_chain"), f"{path}.causal_chain", errors)
            has_wait_relation = any(isinstance(edge, dict) and "wait" in str(edge.get("relation", "")).lower() for edge in chain)
            if not has_wait_relation and not isinstance(obj.get("evidence_gap"), dict):
                errors.append(f"{path} wait conclusion has no real relation and no UnresolvedWaitChain EvidenceGap")
        for hypothesis_index, hypothesis in enumerate(_require_list(obj.get("hypothesis_ledger"), f"{path}.hypothesis_ledger", errors)):
            hypothesis_obj = _require_mapping(hypothesis, f"{path}.hypothesis_ledger[{hypothesis_index}]", errors)
            _required(hypothesis_obj, ["hypothesis", "required_evidence", "mcp_queries", "supporting_evidence", "contradicting_evidence", "status"], f"{path}.hypothesis_ledger[{hypothesis_index}]", errors)
            if hypothesis_obj.get("status") not in {"Supported", "Rejected", "Unresolved"}:
                errors.append(f"{path}.hypothesis_ledger[{hypothesis_index}].status is invalid")

    for index, item in enumerate(_require_list(analysis_obj.get("frame_class_summary"), "analysis.frame_class_summary", errors)):
        item_obj = _require_mapping(item, f"analysis.frame_class_summary[{index}]", errors)
        if item_obj.get("class") not in FRAME_CLASSES:
            errors.append(f"analysis.frame_class_summary[{index}].class is invalid")

    tracy = _require_mapping(analysis_obj.get("tracy_findings"), "analysis.tracy_findings", errors)
    _required(tracy, ["total_capture_overhead", "runtime_findings", "analysis_findings"], "analysis.tracy_findings", errors)
    if tracy.get("total_capture_overhead") != "NotMeasuredSingleTrace":
        errors.append("analysis.tracy_findings.total_capture_overhead must be NotMeasuredSingleTrace")
    _require_list(tracy.get("runtime_findings"), "analysis.tracy_findings.runtime_findings", errors)
    _require_list(tracy.get("analysis_findings"), "analysis.tracy_findings.analysis_findings", errors)

    _require_list(analysis_obj.get("cpu_memory_findings"), "analysis.cpu_memory_findings", errors)
    for index, finding in enumerate(_require_list(analysis_obj.get("gpu_memory_findings"), "analysis.gpu_memory_findings", errors)):
        finding_obj = _require_mapping(finding, f"analysis.gpu_memory_findings[{index}]", errors)
        if finding_obj.get("resource_level") is True and str(finding_obj.get("catalog_status", "")).startswith("invalid"):
            errors.append(
                f"analysis.gpu_memory_findings[{index}] makes a resource-level claim while GPU Catalog is invalid"
            )

    return errors, evidence_by_id, data_by_id


def safe_text(value: Any) -> str:
    return str(value if value is not None else "")


def md_text(value: Any) -> str:
    return (
        safe_text(value)
        .replace("\\", "\\\\")
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace("|", "\\|")
        .replace("\r", " ")
        .replace("\n", " ")
    )


def html_text(value: Any) -> str:
    return html.escape(safe_text(value), quote=True)


def format_ms(value: Any) -> str:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return "Unavailable"
    if math.isfinite(number):
        return f"{number:.3f} ms"
    return "Unavailable"


def stable_id(value: str) -> str:
    clean = re.sub(r"[^A-Za-z0-9_.:-]+", "-", value).strip("-")
    return clean or sha256_bytes(value.encode("utf-8"))[:12]


def js_literal(value: Any) -> str:
    text = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return text.replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026").replace("\u2028", "\\u2028").replace("\u2029", "\\u2029")
