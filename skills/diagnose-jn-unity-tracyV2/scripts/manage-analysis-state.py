#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Maintain recoverable Schema 4 state for Query-native JN Tracy diagnosis.

This utility never opens or scans a Trace. It only validates orchestration
inputs, persists MCP results, constructs the investigation queue, and checks
that AI-authored findings have not altered Query-owned numeric facts.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import re
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


SKILL_ROOT = Path(__file__).resolve().parents[1]
if str(SKILL_ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(SKILL_ROOT / "scripts"))
from report_common import ValidationFailure

VENDOR_ROOT = SKILL_ROOT / "vendor"
if str(VENDOR_ROOT) not in sys.path:
    sys.path.insert(0, str(VENDOR_ROOT))

import yaml  # type: ignore  # vendored PyYAML; SafeLoader only


STATE_SCHEMA_VERSION = 4
PROFILE_SCHEMA_VERSION = 1
REQUIRED_QUERY_SCHEMA = "1.35.0"
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
REQUIRED_PRIORITIES = {"P0", "P1"}
from review_routing import priority as analysis_priority, in_scope as routing_scope


class AnalysisStateError(RuntimeError):
    pass


def _now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def _stable_json(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def _read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise AnalysisStateError(f"cannot read JSON {path}: {exc}") from exc


def _atomic_write(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp.{os.getpid()}")
    # Preserve canonical bytes/atomic replacement without holding another
    # whole JSON string and UTF-8 byte copy beside a large investigation queue.
    with temporary.open("w", encoding="utf-8", newline="\n") as output:
        json.dump(value, output, ensure_ascii=False, indent=2, sort_keys=True)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _require_sha(value: Any, field: str) -> str:
    if not isinstance(value, str) or not SHA256_RE.fullmatch(value):
        raise AnalysisStateError(f"{field} must be a SHA-256 hexadecimal string")
    return value.lower()


def new_state(template_path: Path) -> dict[str, Any]:
    value = _read_json(template_path)
    if not isinstance(value, dict) or value.get("schema_version") != STATE_SCHEMA_VERSION:
        raise AnalysisStateError(f"analysis state template must use schema {STATE_SCHEMA_VERSION}")
    state = copy.deepcopy(value)
    now = _now()
    state["task"]["created_at"] = now
    state["task"]["updated_at"] = now
    return state


def require_profile(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise AnalysisStateError(f"analysis profile is missing: {path}")
    try:
        raw = path.read_bytes()
        if not raw or len(raw) > 1024 * 1024:
            raise AnalysisStateError("analysis profile must be non-empty and at most 1 MiB")
        documents = list(yaml.safe_load_all(raw.decode("utf-8-sig")))
    except (OSError, UnicodeError, yaml.YAMLError) as exc:
        raise AnalysisStateError(f"analysis profile is invalid: {exc}") from exc
    if len(documents) != 1 or not isinstance(documents[0], dict):
        raise AnalysisStateError("analysis profile must contain exactly one mapping document")
    profile = documents[0]
    required = {"schema_version", "profile_name", "frame_budget", "resource_budgets", "candidate_policy", "limits"}
    missing = sorted(required - set(profile))
    if profile.get("schema_version") != PROFILE_SCHEMA_VERSION or missing:
        detail = f"; missing {', '.join(missing)}" if missing else ""
        raise AnalysisStateError(f"analysis profile must use schema {PROFILE_SCHEMA_VERSION}{detail}")
    return profile


def require_query_schema(describe: dict[str, Any]) -> str:
    schema = describe.get("schema_version") or describe.get("query_schema")
    if schema is None and isinstance(describe.get("versions"), dict):
        schema = describe["versions"].get("query_schema")
    if schema != REQUIRED_QUERY_SCHEMA:
        raise AnalysisStateError(
            f"Query schema {REQUIRED_QUERY_SCHEMA} is required; received {schema!r}"
        )
    return schema


def _transition(state: dict[str, Any], target: str, reason: str, evidence: str = "") -> None:
    task = state["task"]
    if task.get("state") == target and task.get("state_history"):
        previous = task["state_history"][-1]
        if previous.get("reason") == reason and previous.get("evidence") == evidence:
            return
    now = _now()
    task["state"] = target
    task["updated_at"] = now
    task["state_history"].append(
        {"state": target, "at": now, "reason": reason, "evidence": evidence}
    )


def apply_scan_status(state: dict[str, Any], response: dict[str, Any], evidence: str = "") -> None:
    if state.get("schema_version") != STATE_SCHEMA_VERSION:
        raise AnalysisStateError("only Schema 4 analysis state can be updated")
    scan_id = response.get("scan_id")
    scan_state = response.get("state")
    progress = response.get("progress")
    if not isinstance(scan_id, str) or not scan_id or not isinstance(scan_state, str):
        raise AnalysisStateError("scan status is missing scan_id/state")
    if not isinstance(progress, dict):
        raise AnalysisStateError("scan status is missing progress")
    existing = state["scan"].get("scan_id")
    if existing and existing != scan_id:
        raise AnalysisStateError("scan status belongs to a different scan")
    state["scan"].update(
        {
            "scan_id": scan_id,
            "state": scan_state,
            "resumable": response.get("resumable") is True,
            "completed": response.get("completed") is True,
            "closed": bool(response.get("closed", False)),
            "progress_completed": str(progress.get("completed", "0")),
            "progress_total": str(progress.get("total", "0")),
            "stage": str(progress.get("stage", "")),
            "error": response.get("error"),
        }
    )
    target = {
        # Query 1.35 wire values; retain legacy Schema 4 spellings below.
        "complete": "scan_complete",
        "cancelled_resumable": "scan_cancelled_resumable",
        "failed": "scan_failed",
        "Complete": "scan_complete",
        "CancelledResumable": "scan_cancelled_resumable",
        "Failed": "scan_failed",
    }.get(scan_state, "scan_running")
    _transition(state, target, f"analysis.scan.status:{scan_state}", evidence)


def next_scan_action(state: dict[str, Any]) -> str:
    scan = state.get("scan", {})
    if scan.get("state") in ("cancelled_resumable", "CancelledResumable") and scan.get("resumable") is True:
        return "resume"
    if scan.get("state") in ("complete", "Complete") and scan.get("completed") is True:
        return "read_summary_and_candidates"
    if scan.get("state") in ("failed", "Failed"):
        return "stop"
    if scan.get("scan_id"):
        return "status"
    return "start"


def apply_scan_summary(state: dict[str, Any], response: dict[str, Any], evidence: str = "") -> None:
    if response.get("policy_algorithm") != "candidate-policy-v3":
        raise AnalysisStateError("scan.policy_algorithm must be candidate-policy-v3")
    scan_id = response.get("scan_id")
    if scan_id != state["scan"].get("scan_id"):
        raise AnalysisStateError("scan summary belongs to a different scan")
    aggregate = _require_sha(response.get("aggregate_content_sha256"), "aggregate_content_sha256")
    candidates = _require_sha(response.get("candidate_content_sha256"), "candidate_content_sha256")
    unchanged = (
        state["scan"].get("aggregate_content_sha256") == aggregate
        and state["scan"].get("candidate_content_sha256") == candidates
    )
    state["scan"]["aggregate_reused"] = bool(state["scan"].get("aggregate_content_sha256") == aggregate)
    state["scan"]["aggregate_content_sha256"] = aggregate
    state["scan"]["candidate_content_sha256"] = candidates
    state["scan"]["summary_evidence"] = evidence
    state["scan"]["policy_algorithm"] = response.get("policy_algorithm", "")
    state["capture_quality"] = copy.deepcopy(response.get("capture_quality", []))
    if not unchanged:
        _transition(state, "candidate_manifest_pending", "analysis.scan.summary", evidence)


def _queue_item(candidate: dict[str, Any], required: bool) -> dict[str, Any]:
    return {
        "candidate_id": candidate["candidate_id"],
        "priority": candidate.get("priority", "P4"),
        "analysis_priority": analysis_priority(candidate),
        "domain": candidate.get("domain", "unknown"),
        "signature_id": candidate.get("signature_id", ""),
        "triggers": list(candidate.get("triggers", [])),
        "required": required,
        "query_selected": bool(candidate.get("selected", False)),
        "status": "pending",
        "frame_scope": candidate.get("frame_scope", ""),
        "thread_or_queue": candidate.get("thread_or_queue", ""),
        "manifestation": candidate.get("manifestation", ""),
        "observation_unit": candidate.get("observation_unit", "frame"),
    }


def _candidate_sort_key(candidate: dict[str, Any]) -> tuple[int, int, int, str]:
    required = analysis_priority(candidate) == 'P0'
    priority = {"P0": 0, "P1": 1, "P2": 2, "P3": 3, "P4": 4}.get(analysis_priority(candidate), 5)
    focus = 0 if "user_focus" in candidate.get("triggers", []) else 1
    return (0 if required else 1), priority, focus, str(candidate.get("candidate_id", ""))


def ingest_candidate_pages(
    state: dict[str, Any], pages: Iterable[dict[str, Any]], evidence_paths: Iterable[str] | None = None
) -> None:
    # Keep compact queue metadata, not all pages/trigger facts/representatives.
    # Full Query records remain in the immutable page evidence. This removes
    # raw-payload amplification; queue metadata still grows with candidate IDs.
    compact_candidates: list[dict[str, Any]] = []
    expected_total: int | None = None
    seen_ids: set[str] = set()
    page_count = 0
    done = False
    for index, page in enumerate(pages):
        if done:
            raise AnalysisStateError("candidate pages continue after done")
        items = page.get("items")
        metadata = page.get("page")
        if not isinstance(items, list) or not isinstance(metadata, dict):
            raise AnalysisStateError(f"candidate page {index} is malformed")
        if any(container.get(flag) for container in (page, metadata) for flag in ("partial", "truncated")):
            raise AnalysisStateError("candidate pagination is partial/truncated")
        try:
            total = int(metadata.get("total"))
            cursor = metadata.get("cursor")
            # Old saved single-page fixtures omit the ordinal. Actual Query
            # pages use decimal cursors, whose continuity must be verified.
            if cursor not in (None, "") and int(cursor) != len(seen_ids):
                raise AnalysisStateError("candidate cursor ordinal mismatch")
        except (TypeError, ValueError) as exc:
            raise AnalysisStateError(f"candidate page {index} has invalid total") from exc
        if total < 0:
            raise AnalysisStateError("candidate total must not be negative")
        if expected_total is None:
            expected_total = total
        elif expected_total != total:
            raise AnalysisStateError("candidate page totals disagree")
        for candidate in items:
            if not isinstance(candidate, dict) or not isinstance(candidate.get("candidate_id"), str):
                raise AnalysisStateError(f"candidate page {index} contains an invalid candidate")
            candidate_id = candidate["candidate_id"]
            if candidate.get("domain") == "quality" or "quality" in candidate.get("triggers", []):
                raise AnalysisStateError("legacy quality candidate: rerun policy using candidate-policy-v2; do not investigate")
            if candidate_id in seen_ids:
                raise AnalysisStateError(f"duplicate candidate_id: {candidate_id}")
            seen_ids.add(candidate_id)
            triggers = candidate.get("triggers", [])
            required = routing_scope(candidate)
            compact_candidates.append(_queue_item(candidate, required))
        page_count += 1
        done = metadata.get("done") is True
        next_cursor = metadata.get("next_cursor")
        if done:
            if next_cursor is not None:
                raise AnalysisStateError("completed candidate page still has a cursor")
        else:
            try:
                if not items or int(next_cursor) != len(seen_ids):
                    raise AnalysisStateError("candidate pagination did not advance exactly")
            except (TypeError, ValueError) as exc:
                raise AnalysisStateError("candidate next cursor is invalid") from exc
    if not page_count:
        raise AnalysisStateError("candidate pagination returned no pages")
    if not done:
        raise AnalysisStateError("candidate pagination is incomplete")
    if expected_total != len(compact_candidates):
        raise AnalysisStateError(
            f"candidate pagination count mismatch: expected {expected_total}, got {len(compact_candidates)}"
        )
    candidate_count = len(compact_candidates)
    del seen_ids

    completed = {
        item.get("candidate_id")
        for item in state["investigations"].get("completed", [])
        if isinstance(item, dict)
    }
    if state.get("scan", {}).get("policy_algorithm") != "candidate-policy-v3":
        raise AnalysisStateError("scan.policy_algorithm must be candidate-policy-v3")
    if completed or state["investigations"].get("routing_workflow") == "2.2.0":
        audit_errors = validate_investigation_audit(state)
        if audit_errors:
            raise AnalysisStateError("cannot resume unverified completed IDs: " + "; ".join(audit_errors))
    if state["investigations"].get("routing_workflow") == "2.2.0":
        from candidate_manifest import load_candidate_manifest
        audit_path=Path(state["investigations"]["artifact_audit"]["candidates"]["path"])
        expected=load_candidate_manifest(audit_path)
        actual={item["candidate_id"]:item for item in compact_candidates}
        count=0
        for candidate in expected["candidates"]:
            count+=1
            if actual.get(candidate["candidate_id"]) != _queue_item(candidate,routing_scope(candidate)):
                raise AnalysisStateError("resumed pages differ from audited routing candidates")
        if count!=candidate_count:
            raise AnalysisStateError("resumed candidate count differs from routing audit")
        # Preserve separately audited query completion, tester decisions and source queue.
        return
    queue: list[dict[str, Any]] = []
    backlog: list[dict[str, Any]] = []
    required_total = 0
    required_completed = 0
    compact_candidates.sort(key=_candidate_sort_key)
    for item in compact_candidates:
        required = item["required"]
        if required:
            required_total += 1
        if item["candidate_id"] in completed:
            if required:
                required_completed += 1
            continue
        backlog.append(item)
        if required:
            queue.append(copy.deepcopy(item))

    state["investigations"]["queue"] = queue
    state["investigations"]["backlog"] = backlog
    state["investigations"]["coverage"] = {
        "candidate_total": candidate_count,
        "required_total": required_total,
        "required_completed": required_completed,
        "investigated_total": len(completed),
        "uninvestigated_total": len(backlog),
    }
    evidence = list(evidence_paths or [])
    state["candidate_manifest"].update(
        {
            "page_evidence": evidence,
            "page_count": page_count,
            "candidate_count": candidate_count,
            "pagination_complete": True,
            "content_sha256": state["scan"].get("candidate_content_sha256", ""),
        }
    )
    _transition(state, "investigating_candidates", "candidate_manifest_complete", evidence[-1] if evidence else "")


def validate_candidate_numeric_facts(
    analysis: dict[str, Any], candidate_manifest: dict[str, Any]
) -> list[str]:
    errors: list[str] = []
    candidates = candidate_manifest.get("candidates")
    findings = analysis.get("candidate_findings")
    from candidate_manifest import is_candidate_collection
    if not is_candidate_collection(candidates):
        return ["candidate_manifest.candidates must be an array"]
    if not isinstance(findings, list):
        return ["analysis.candidate_findings must be an array"]
    finding_ids = {item.get("candidate_id") for item in findings
                   if isinstance(item, dict) and isinstance(item.get("candidate_id"), str)}
    by_id = {
        item.get("candidate_id"): item
        for item in candidates
        if isinstance(item, dict) and item.get("candidate_id") in finding_ids
    }
    seen: set[str] = set()
    for index, finding in enumerate(findings):
        if not isinstance(finding, dict):
            errors.append(f"analysis.candidate_findings[{index}] must be an object")
            continue
        candidate_id = finding.get("candidate_id")
        if candidate_id in seen:
            errors.append(f"analysis.candidate_findings[{index}] duplicates candidate {candidate_id}")
        seen.add(candidate_id)
        candidate = by_id.get(candidate_id)
        if candidate is None:
            errors.append(f"analysis.candidate_findings[{index}] references unknown candidate {candidate_id}")
            continue
        if finding.get("query_numeric_facts") != candidate.get("trigger_evidence", []):
            errors.append(
                f"analysis.candidate_findings[{index}].query_numeric_facts differs from Candidate Manifest"
            )
    return errors


def add_ledger_entry(state: dict[str, Any], kind: str, entry: dict[str, Any]) -> None:
    key = {"mcp": "mcp_queries", "source": "source_evidence", "hypothesis": "hypotheses"}.get(kind)
    if key is None:
        raise AnalysisStateError(f"unknown ledger kind: {kind}")
    identity = entry.get("id")
    if not isinstance(identity, str) or not identity:
        raise AnalysisStateError("ledger entry requires a stable id")
    ledger = state["ledgers"][key]
    existing = next((item for item in ledger if item.get("id") == identity), None)
    if existing is not None:
        if existing != entry:
            raise AnalysisStateError(f"ledger id {identity} already has different content")
        return
    ledger.append(copy.deepcopy(entry))


def validate_investigation_audit(state: dict[str, Any]) -> list[str]:
    """Recheck saved artifacts, rather than trusting a manually completed ID."""
    audit = state.get("investigations", {}).get("artifact_audit")
    if not isinstance(audit, dict):
        return ["v3 investigation artifact audit is missing"]
    try:
        docs = {}
        for key in ("analysis", "candidates", "evidence"):
            path = Path(audit[key]["path"])
            if not path.is_absolute() or _sha256_file(path) != audit[key]["sha256"]:
                return [f"investigation audit {key} artifact identity mismatch"]
            if key == "candidates":
                from candidate_manifest import load_candidate_manifest
                docs[key] = load_candidate_manifest(path)
            else:
                docs[key] = _read_json(path)
        if docs["candidates"].get("content_sha256") != state.get("scan", {}).get("candidate_content_sha256"):
            return ["investigation audit belongs to another candidate manifest"]
        script_root = str(Path(__file__).resolve().parent)
        if script_root not in sys.path:
            sys.path.insert(0, script_root)
        from query_native_report import validate_query_native_analysis
        from report_common import validate_analysis_and_evidence
        errors = validate_analysis_and_evidence(docs["analysis"], docs["evidence"], Path(audit["evidence"]["path"]).parent)[0]
        errors += validate_query_native_analysis(docs["analysis"], docs["candidates"], docs["evidence"], Path(audit["evidence"]["path"]).parent)
        from performance_review import validate_review
        deep_errors, deep_model = validate_review(docs["analysis"], docs["candidates"], docs["evidence"], Path(audit["evidence"]["path"]).parent)
        errors.extend(deep_errors)
        completed = {item.get("candidate_id") for item in state["investigations"].get("completed", [])}
        pending_deep = set(deep_model.get("pending", []))
        if docs["analysis"].get("report_versions", {}).get("analysis_workflow") == "2.2.0":
            from review_routing import validate_routing
            routes, _ = validate_routing(docs["analysis"], docs["candidates"], docs["evidence"], Path(audit["evidence"]["path"]).parent)
            query_done = {f["candidate_id"] for f in docs["analysis"].get("candidate_findings", [])}
            filtered = {cid for cid,row in routes.items() if row["route"] == "filtered"}
            waiting = {cid for cid,row in routes.items() if row["decision"] == "pending"}
            for key, expected in (("query_reviewed_ids",query_done),("filtered_ids",filtered),("tester_pending_ids",waiting)):
                if set(state["investigations"].get(key,[])) != expected:
                    errors.append("routing state differs from audited " + key)
            if state.get("task",{}).get("state") in {"completed","completed_with_degradation"} and pending_deep:
                errors.append("completed state has pending mandatory or approved source analysis")
        if completed & pending_deep:
            errors.append("completed IDs include candidates without completed source analysis")
        verified = set(deep_model.get("deep_completed_ids", []))
        if completed != verified:
            errors.append("completed IDs differ from artifact audit findings")
        if state.get("task", {}).get("state") in {"completed", "completed_with_degradation"}:
            coverage = docs["analysis"].get("query_scan", {}).get("coverage", {})
            if coverage != state.get("investigations", {}).get("coverage", {}):
                errors.append("completed coverage differs from artifact audit")
            if (docs["analysis"].get("report_status", "complete") != "complete"
                    or coverage.get("required_completed") != coverage.get("required_total")):
                errors.append("completed state requires a complete audited report")
        return errors
    except (KeyError, TypeError, ValueError, OSError, ValidationFailure) as exc:
        return [f"investigation artifact audit failed: {exc}"]


def validate_state(state: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if state.get("schema_version") != STATE_SCHEMA_VERSION:
        errors.append(f"state.schema_version must be {STATE_SCHEMA_VERSION}")
    query = state.get("query", {})
    if query.get("schema") and query.get("schema") != REQUIRED_QUERY_SCHEMA:
        errors.append(f"state.query.schema must be {REQUIRED_QUERY_SCHEMA}")
    candidate = state.get("candidate_manifest", {})
    if candidate.get("page_count", 0) and candidate.get("pagination_complete") is not True:
        errors.append("candidate pagination is incomplete")
    investigations = state.get("investigations", {})
    completed = {item.get("candidate_id") for item in investigations.get("completed", []) if isinstance(item, dict)}
    queued = [item.get("candidate_id") for item in investigations.get("queue", []) if isinstance(item, dict)]
    if len(queued) != len(set(queued)):
        errors.append("investigation queue contains duplicate candidates")
    if completed.intersection(queued):
        errors.append("completed candidates remain in the investigation queue")
    if state.get("task", {}).get("state") in {"completed", "completed_with_degradation"}:
        if state.get("scan", {}).get("policy_algorithm") != "candidate-policy-v3":
            errors.append("scan.policy_algorithm must be candidate-policy-v3")
        errors.extend(validate_investigation_audit(state))
        coverage = investigations.get("coverage", {})
        pending = [item for item in investigations.get("backlog", [])
                   if isinstance(item, dict) and (item.get("required") or item.get("query_selected"))
                   and item.get("candidate_id") not in completed
                   and item.get("candidate_id") not in set(investigations.get("query_reviewed_ids", []))
                   and item.get("candidate_id") not in set(investigations.get("filtered_ids", []))]
        if queued or pending or coverage.get("required_completed") != coverage.get("required_total"):
            errors.append("completed state has pending required/selected investigations; save an in-progress report")
    return errors


def _load_state(path: Path) -> dict[str, Any]:
    state = _read_json(path)
    if not isinstance(state, dict) or state.get("schema_version") != STATE_SCHEMA_VERSION:
        raise AnalysisStateError(f"state must use schema {STATE_SCHEMA_VERSION}")
    return state


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    initialize = subparsers.add_parser("init")
    initialize.add_argument("--template", type=Path, default=SKILL_ROOT / "assets" / "analysis-state-template.json")
    initialize.add_argument("--state", type=Path, required=True)
    initialize.add_argument("--profile", type=Path, required=True)
    initialize.add_argument("--query-describe", type=Path, required=True)

    for name in ("scan-status", "scan-summary"):
        command = subparsers.add_parser(name)
        command.add_argument("--state", type=Path, required=True)
        command.add_argument("--response", type=Path, required=True)
        command.add_argument("--evidence", default="")

    ingest = subparsers.add_parser("ingest-candidates")
    ingest.add_argument("--state", type=Path, required=True)
    ingest.add_argument("--page", type=Path, action="append", required=True)

    ledger = subparsers.add_parser("record-ledger")
    ledger.add_argument("--state", type=Path, required=True)
    ledger.add_argument("--kind", choices=["mcp", "source", "hypothesis"], required=True)
    ledger.add_argument("--entry", type=Path, required=True)

    validate = subparsers.add_parser("validate")
    validate.add_argument("--state", type=Path, required=True)

    audit = subparsers.add_parser("record-investigation-audit")
    for name in ("state", "analysis", "candidates", "evidence"):
        audit.add_argument("--" + name, type=Path, required=True)

    numeric = subparsers.add_parser("validate-numerics")
    numeric.add_argument("--analysis", type=Path, required=True)
    numeric.add_argument("--candidate-manifest", type=Path, required=True)

    args = parser.parse_args()
    try:
        if args.command == "init":
            require_profile(args.profile)
            describe = _read_json(args.query_describe)
            state = new_state(args.template)
            state["profile"]["authoring_path"] = str(args.profile.resolve())
            state["profile"]["authoring_sha256"] = _sha256_file(args.profile)
            state["query"]["schema"] = require_query_schema(describe)
            _transition(state, "profile_validated", "profile_preflight")
            _atomic_write(args.state, state)
        elif args.command in {"scan-status", "scan-summary"}:
            state = _load_state(args.state)
            response = _read_json(args.response)
            if args.command == "scan-status":
                apply_scan_status(state, response, args.evidence)
            else:
                apply_scan_summary(state, response, args.evidence)
            _atomic_write(args.state, state)
        elif args.command == "ingest-candidates":
            state = _load_state(args.state)
            pages = (_read_json(path) for path in args.page)
            ingest_candidate_pages(state, pages, [str(path) for path in args.page])
            _atomic_write(args.state, state)
        elif args.command == "record-ledger":
            state = _load_state(args.state)
            entry = _read_json(args.entry)
            add_ledger_entry(state, args.kind, entry)
            _atomic_write(args.state, state)
        elif args.command == "record-investigation-audit":
            state = _load_state(args.state)
            state["investigations"]["artifact_audit"] = {
                key: {"path": str(getattr(args, key).resolve()), "sha256": _sha256_file(getattr(args, key))}
                for key in ("analysis", "candidates", "evidence")
            }
            analysis = _read_json(args.analysis)
            deep_completed = {cid for issue in analysis.get("performance_review", {}).get("issues", [])
                              if issue.get("analysis_state") == "complete" for cid in issue.get("candidate_ids", [])}
            deep_completed -= {cid for issue in analysis.get("performance_review", {}).get("issues", [])
                               if issue.get("analysis_state") != "complete" for cid in issue.get("candidate_ids", [])}
            state["investigations"]["completed"] = [
                {"candidate_id": item["candidate_id"], "status": item["conclusion_status"]}
                for item in analysis.get("candidate_findings", []) if item["candidate_id"] in deep_completed]
            if analysis.get("report_versions", {}).get("analysis_workflow") == "2.2.0":
                from candidate_manifest import load_candidate_manifest
                from review_routing import validate_routing, deep_required, query_required
                manifest = load_candidate_manifest(args.candidates)
                routes, _ = validate_routing(analysis, manifest, _read_json(args.evidence), args.evidence.parent)
                done = {f["candidate_id"] for f in analysis.get("candidate_findings", [])}
                state["investigations"]["routing_workflow"] = "2.2.0"
                state["investigations"]["query_reviewed_ids"] = sorted(done)
                state["investigations"]["filtered_ids"] = sorted(cid for cid,row in routes.items() if row["route"] == "filtered")
                state["investigations"]["tester_pending_ids"] = sorted(cid for cid,row in routes.items() if row["decision"] == "pending")
                queue = []
                for c in manifest["candidates"]:
                    cid = c["candidate_id"]
                    if (deep_required(c,routes) and cid not in deep_completed) or (query_required(c,routes) and cid not in done):
                        queue.append(_queue_item(c, True))
                state["investigations"]["queue"] = sorted(queue, key=_candidate_sort_key)
                state["investigations"]["coverage"] = analysis["query_scan"]["coverage"]
            errors = validate_investigation_audit(state)
            if errors:
                raise AnalysisStateError("; ".join(errors))
            _atomic_write(args.state, state)
        elif args.command == "validate":
            errors = validate_state(_load_state(args.state))
            print(json.dumps({"passed": not errors, "errors": errors}, ensure_ascii=False))
            return 0 if not errors else 1
        elif args.command == "validate-numerics":
            from candidate_manifest import load_candidate_manifest
            errors = validate_candidate_numeric_facts(
                _read_json(args.analysis), load_candidate_manifest(args.candidate_manifest)
            )
            print(json.dumps({"passed": not errors, "errors": errors}, ensure_ascii=False))
            return 0 if not errors else 1
        return 0
    except (AnalysisStateError, ValidationFailure) as exc:
        print(json.dumps({"passed": False, "error": str(exc)}, ensure_ascii=False), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
