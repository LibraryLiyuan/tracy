"""Validate saved MCP receipts; never scan Trace data or invent numeric facts.

This is an audit of recorded artifacts, not cryptographic proof against forged
files. It prevents ordinary workflow mistakes: absent queries, stale replies,
wrong frames/threads, prose-only completion and unrelated same-name events.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any


def _file(root: Path, item: dict) -> dict:
    relative = Path(item["path"])
    path = (root / relative).resolve()
    if relative.is_absolute() or not path.is_relative_to(root.resolve()):
        raise ValueError("receipt_path_outside_evidence_root")
    if path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("receipt_requires_pagination")
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != item["sha256"].lower():
        raise ValueError("receipt_checksum_mismatch")
    result = json.loads(raw)
    if not isinstance(result, dict):
        raise ValueError("receipt_not_object")
    return result


def _pointer(value: Any, pointer: str) -> Any:
    if not pointer.startswith("/"):
        raise ValueError("invalid_json_pointer")
    for component in pointer[1:].split("/"):
        key = component.replace("~1", "/").replace("~0", "~")
        value = value[int(key)] if isinstance(value, list) else value[key]
    return value


def _receipt(root: Path, item: dict, trace_id: str, fingerprint: str, allow_unavailable=False,
             allow_pagination=False, identity_anchor=None) -> tuple[str, dict, dict]:
    raw = item.get("mcp_receipt", item)
    request, response = _file(root, raw["request"]), _file(root, raw["response"])
    if request.get("jsonrpc") != "2.0" or request.get("method") != "tools/call":
        raise ValueError("receipt_is_not_mcp_tools_call")
    if request.get("id") is None or response.get("id") != request["id"]:
        raise ValueError("receipt_request_response_id_mismatch")
    args = request["params"]["arguments"]
    if args.get("trace_id") != trace_id:
        raise ValueError("receipt_trace_id_mismatch")
    if request["params"].get("name") != "tracy_inspect":
        raise ValueError("receipt_not_directed_inspection")
    structured = response.get("result", {}).get("structuredContent")
    if not isinstance(structured, dict):
        raise ValueError("receipt_missing_structured_content")
    failed = response.get("error") or response.get("result", {}).get("isError") or not structured.get("ok")
    actual_fingerprint = structured.get("trace", {}).get("fingerprint", "")
    if not actual_fingerprint and failed and allow_unavailable and identity_anchor is not None:
        # Query Failure has no trace envelope. Bind it through a saved successful
        # query; never modify the original error response or trust trace_id alone.
        _, _, anchor_content = _receipt(root, identity_anchor, trace_id, fingerprint)
        actual_fingerprint = anchor_content["trace"]["fingerprint"]
    if len(fingerprint) != 64 or actual_fingerprint.lower() != fingerprint.lower():
        raise ValueError("receipt_source_fingerprint_mismatch")
    if response.get("error") or response.get("result", {}).get("isError") or not structured.get("ok"):
        error = structured.get("error", response.get("error", {}))
        code = str(error.get("code", "")).upper() if isinstance(error, dict) else ""
        # Invalid arguments, wrong IDs, budget exhaustion and arbitrary errors
        # are not evidence that the source capability is absent.
        if not allow_unavailable or code not in {
            "CAPABILITY_UNAVAILABLE", "DOMAIN_UNAVAILABLE", "NOT_CAPTURED",
            "UNSUPPORTED_BACKEND", "SOURCE_DOMAIN_INVALID", "INSUFFICIENT_EVIDENCE",
        }:
            raise ValueError("receipt_query_failed_actual_error:" + json.dumps(error))
        structured = dict(structured, _unavailable_code=code, _request_id=request["id"])
    budget = structured.get("budget", {})
    if not isinstance(budget, dict):
        raise ValueError("receipt_invalid_budget")
    incomplete = (structured.get("partial") or structured.get("truncated")
                  or budget.get("exhausted_by"))
    if args.get("params", {}).get("cursor") and not allow_pagination:
        incomplete = True  # An exhausted last page is not a complete query.
    containers = [structured]
    if isinstance(structured.get("data"), dict):
        containers.append(structured["data"])
    # Some Query methods expose pagination in data rather than the envelope.
    # A missing cursor does not prove exhaustion when has_more is true.
    for container in containers:
        for key in ("page", "pagination"):
            page = container.get(key)
            if page is not None:
                if not isinstance(page, dict):
                    raise ValueError("receipt_invalid_pagination")
                incomplete = (incomplete or page.get("partial") or page.get("truncated")
                              or (not allow_pagination and (page.get("next_cursor") or page.get("has_more"))))
    if incomplete:
        raise ValueError("receipt_query_incomplete")
    return args["method"], args.get("params", {}), structured


def _receipt_chain(root, receipt, by_id, trace_id, fingerprint):
    """Validate an explicitly ordered whole query without rewriting any reply."""
    ids = receipt["page_evidence_ids"]
    if (not isinstance(ids, list) or not ids or len(ids) > 128
            or ids[0] != receipt["evidence_id"] or len(set(ids)) != len(ids)
            or receipt.get("outcome", "available") not in {"available", "verified_not_recorded"}):
        raise ValueError("receipt_invalid_page_chain")
    first = None
    previous_cursor = None
    seen_cursors, seen_requests = set(), set()
    for index, eid in enumerate(ids):
        item = by_id[eid]
        method, params, content = _receipt(root, item, trace_id, fingerprint, allow_pagination=True)
        if not isinstance(content.get("data"), dict):
            raise ValueError("receipt_page_data_not_object")
        request = _file(root, item.get("mcp_receipt", item)["request"])
        if request["id"] in seen_requests:
            raise ValueError("receipt_page_request_repeated")
        seen_requests.add(request["id"])
        comparable = {k: v for k, v in params.items() if k != "cursor"}
        if index == 0:
            if params.get("cursor"):
                raise ValueError("receipt_page_chain_missing_initial_page")
            first = method, params, content
            original_params = comparable
            original_trace = content.get("trace")
        elif (method != first[0] or comparable != original_params
              or content.get("trace") != original_trace or not previous_cursor
              or params.get("cursor") != previous_cursor):
            raise ValueError("receipt_page_chain_identity_or_cursor_mismatch")
        # Trees repeat their root on every page. It must remain the same entity
        # and timing state; unrelated child pages cannot supply coverage.
        if content.get("data", {}).get("root") != first[2].get("data", {}).get("root"):
            raise ValueError("receipt_page_tree_root_changed")
        pages = []
        for container in (content, content.get("data", {})):
            if isinstance(container, dict):
                pages.extend(container[key] for key in ("page", "pagination") if key in container)
        if not pages:
            raise ValueError("receipt_page_chain_missing_page_metadata")
        cursors = {p["next_cursor"] for p in pages if p.get("next_cursor")}
        if len(cursors) > 1:
            raise ValueError("receipt_page_chain_ambiguous_cursor")
        cursor = next(iter(cursors), None)
        if any(p.get("has_more") for p in pages) and not cursor:
            raise ValueError("receipt_query_incomplete")
        if index < len(ids) - 1 and not cursor:
            raise ValueError("receipt_page_chain_ends_early")
        if cursor in seen_cursors or (index == len(ids) - 1 and cursor):
            raise ValueError("receipt_page_chain_not_exhausted")
        if (index == len(ids) - 1 and not any(
                ("next_cursor" in p and p["next_cursor"] is None) or p.get("has_more") is False
                for p in pages)):
            raise ValueError("receipt_page_chain_exhaustion_not_explicit")
        if cursor:
            seen_cursors.add(cursor)
        previous_cursor = cursor
    return first


def _verified_empty_frame(candidate, finding, receipt, method, params, content, by_id, trace, fingerprint, root):
    """Only a full empty FrameSet interval can replace an unavailable root tree."""
    scope = candidate.get("frame_scope")
    if (candidate.get("domain", "cpu") != "cpu" or candidate.get("observation_unit", "frame") != "frame"
            or not scope or candidate.get("signature_id") != "frame-wall:" + scope
            or finding.get("conclusion_status") != "Unresolved" or receipt.get("purpose") != "zone_tree"
            or method != "zone.cpu.search" or receipt.get("entity_json_pointers")):
        raise ValueError("receipt_absence_only_for_unresolved_frame_duration")
    # Reject every narrowing filter, including unsupported parameters that Query
    # might ignore. An empty last page cannot prove that earlier pages were empty.
    allowed = {"start_ns", "end_ns", "limit", "max_cpu_ms", "max_scan_events", "max_nodes", "max_edges", "max_groups"}
    if set(params) - allowed or not {"start_ns", "end_ns"} <= set(params):
        raise ValueError("receipt_absence_requires_unfiltered_initial_query")
    page, budget = content.get("page", {}), content.get("budget", {})
    if not isinstance(page, dict) or not isinstance(budget, dict):
        raise ValueError("receipt_absence_invalid_page_or_budget")
    if (content.get("data", {}).get("zones") != [] or content.get("partial") is not False
            or str(content.get("omitted_count")) != "0" or page.get("returned") != 0
            or "next_cursor" not in page or page["next_cursor"] is not None
            or page.get("partial") is not False or page.get("truncated") is not False
            or str(page.get("omitted_count")) != "0" or page.get("omitted_count_exact") is not True
            or budget.get("exhausted_by") != [] or budget.get("omitted_count_exact") is not True):
        raise ValueError("receipt_absence_requires_complete_empty_query")
    amethod, aparams, anchor = _receipt(root, by_id[receipt["frame_anchor_evidence_id"]], trace["trace_id"], fingerprint)
    frame = _pointer(anchor, receipt.get("frame_anchor_json_pointer", "/data"))
    index = frame.get("index", frame.get("frame_index"))
    request_scope = aparams.get("frame_set", aparams.get("frame_set_ref"))
    # frame.get accepts a FrameSet name as well as an opaque ref. The resolved
    # response ref remains authoritative; only the Query candidate's own name
    # can be an alternative request spelling, never an arbitrary alias.
    if (amethod != "frame.get" or request_scope not in {scope, candidate.get("structural_signature")}
            or frame.get("frame_set_ref", frame.get("frame_scope")) != scope
            or index is None or str(aparams.get("index")) != str(index)
            or str(index) not in set(map(str, candidate.get("representative_frames", [])))
            or int(frame["begin_ns"]) >= int(frame["end_ns"])
            or int(params["start_ns"]) != int(frame["begin_ns"])
            or int(params["end_ns"]) != int(frame["end_ns"])):
        raise ValueError("receipt_absence_frame_identity_or_bounds_mismatch")
    return str(index)


_BUDGET_PARAMS = {"limit", "max_cpu_ms", "max_scan_events", "max_nodes", "max_edges", "max_groups"}


def _same_revision(left, right):
    if left.get("trace", {}).get("revision") != right.get("trace", {}).get("revision"):
        raise ValueError("receipt_evidence_revision_mismatch")


def _complete_collection_page(content, key, last=True):
    """Count/list agreement is a receipt integrity check, not a Trace statistic."""
    data, page, budget = content.get("data", {}), content.get("page"), content.get("budget")
    values = data.get(key) if isinstance(data, dict) else None
    if (not isinstance(values, list) or not isinstance(page, dict) or not isinstance(budget, dict)
            or content.get("partial") is not False or content.get("truncated") is True
            or isinstance(page.get("returned"), bool) or page.get("returned") != len(values)
            or page.get("partial") is not False or page.get("truncated") is not False
            or page.get("omitted_count_exact") is not True
            or budget.get("exhausted_by") != [] or budget.get("omitted_count_exact") is not True):
        raise ValueError("receipt_absence_requires_complete_collection")
    if last and (str(content.get("omitted_count")) != "0" or str(page.get("omitted_count")) != "0"
                 or "next_cursor" not in page or page["next_cursor"] is not None
                 or page.get("has_more") is True):
        raise ValueError("receipt_absence_requires_exhausted_collection")
    return values


def _candidate_frame(candidate, receipt, by_id, trace, fingerprint, root, content):
    amethod, aparams, anchor = _receipt(root, by_id[receipt["frame_anchor_evidence_id"]], trace["trace_id"], fingerprint)
    _same_revision(anchor, content)
    frame = _pointer(anchor, receipt.get("frame_anchor_json_pointer", "/data"))
    if not isinstance(frame, dict):
        raise ValueError("receipt_candidate_frame_not_object")
    scope, request_scope = candidate.get("frame_scope"), aparams.get("frame_set", aparams.get("frame_set_ref"))
    index = frame.get("index", frame.get("frame_index"))
    if (amethod != "frame.get" or not scope or not isinstance(frame, dict)
            or frame.get("frame_set_ref", frame.get("frame_scope")) != scope
            or frame.get("complete") is not True or index is None
            or str(aparams.get("index")) != str(index)
            or str(index) not in set(map(str, candidate.get("representative_frames", [])))
            or int(frame["begin_ns"]) >= int(frame["end_ns"])):
        raise ValueError("receipt_candidate_frame_identity_mismatch")
    if request_scope != scope:
        # A named frame.get is legitimate only with an independent definition;
        # a caller-supplied name or opaque-ID suffix cannot establish the alias.
        smethod, _, sets = _receipt(root, by_id[receipt["frame_set_evidence_id"]], trace["trace_id"], fingerprint)
        _same_revision(anchor, sets)
        if not isinstance(sets.get("data"), dict) or not isinstance(sets["data"].get("frame_sets"), list):
            raise ValueError("receipt_frame_set_definitions_invalid")
        item = _pointer(sets, receipt["frame_set_json_pointer"])
        if (smethod != "frame.sets" or not isinstance(item, dict) or item not in sets["data"]["frame_sets"]
                or item.get("ref") != scope or item.get("name") != request_scope):
            raise ValueError("receipt_frame_set_alias_mismatch")
    return str(index), frame


def _exclusion_path(node, proof, target, by_id, trace, fingerprint, root, content):
    chain = []
    for entry in proof.get("parent_chain_evidence", []):
        method, params, parent_content = _receipt(root, by_id[entry["evidence_id"]], trace["trace_id"], fingerprint)
        _same_revision(parent_content, content)
        parent = _pointer(parent_content, entry.get("entity_json_pointer", "/data"))
        if (method != "zone.cpu.get" or not isinstance(parent, dict) or not parent.get("ref")
                or params.get("ref") != parent["ref"] or parent.get("thread_ref") != target
                or not isinstance(parent.get("name"), str) or "parent_ref" not in parent):
            raise ValueError("receipt_exclusion_parent_identity_mismatch")
        chain.append(parent)
    linked = chain + [node]
    refs = [item.get("ref") for item in linked]
    # Open ancestor timing is retained: only the full structural chain is used.
    # No duration or completion claim is derived from an open ancestor.
    if ((not chain and ("parent_ref" not in node or node["parent_ref"] is not None))
            or (chain and chain[0]["parent_ref"] is not None) or None in refs or len(set(refs)) != len(refs)
            or not all(child.get("parent_ref") == parent["ref"] for parent, child in zip(linked, linked[1:]))):
        raise ValueError("receipt_exclusion_parent_chain_incomplete")
    return " > ".join(item["name"] for item in linked)


def _verified_candidate_not_recorded(candidate, finding, receipt, method, params, content, by_id, trace, fingerprint, root):
    scope, target, path = candidate.get("frame_scope"), candidate.get("thread_or_queue"), candidate.get("structural_signature", "")
    if (candidate.get("domain") != "cpu" or candidate.get("observation_unit", "frame") != "frame"
            or not scope or not target or not path or candidate.get("signature_id") == "frame-wall:" + scope
            or finding.get("conclusion_status") != "Unresolved" or receipt.get("purpose") != "zone_tree"
            or method != "zone.cpu.search" or receipt.get("entity_json_pointers")):
        raise ValueError("receipt_not_recorded_only_for_unresolved_cpu_candidate")
    allowed = _BUDGET_PARAMS | {"start_ns", "end_ns", "filter"}
    if set(params) - allowed or not {"start_ns", "end_ns"} <= set(params):
        raise ValueError("receipt_not_recorded_unsupported_search_filter")
    leaf = path.split(" > ")[-1]
    if "filter" in params and params["filter"] != {"mode": "exact", "text": leaf}:
        raise ValueError("receipt_not_recorded_search_must_cover_exact_leaf")
    frame_index, frame = _candidate_frame(candidate, receipt, by_id, trace, fingerprint, root, content)
    if int(params["start_ns"]) != int(frame["begin_ns"]) or int(params["end_ns"]) != int(frame["end_ns"]):
        raise ValueError("receipt_not_recorded_search_bounds_mismatch")
    ids = receipt.get("page_evidence_ids", [receipt["evidence_id"]])
    proofs = receipt.get("exclusion_evidence", [])
    proof_map = {entry["ref"]: entry for entry in proofs}
    if len(proof_map) != len(proofs):
        raise ValueError("receipt_exclusion_duplicate_ref")
    used_proofs, seen = set(), set()
    for i, eid in enumerate(ids):
        if i == 0:
            page_content = content
        else:
            _, _, page_content = _receipt(root, by_id[eid], trace["trace_id"], fingerprint, allow_pagination=True)
        for node in _complete_collection_page(page_content, "zones", last=i == len(ids) - 1):
            if (not isinstance(node, dict) or not node.get("ref") or node["ref"] in seen
                    or not isinstance(node.get("name"), str) or not node.get("thread_ref")):
                raise ValueError("receipt_not_recorded_search_node_unresolved")
            seen.add(node["ref"])
            if node["name"] != leaf:
                if "filter" in params:
                    raise ValueError("receipt_not_recorded_filter_result_mismatch")
                continue
            if node["thread_ref"] != target:
                continue
            if node.get("signature_id") == candidate.get("signature_id"):
                raise ValueError("receipt_not_recorded_candidate_was_returned")
            actual_path = node.get("path")
            if not isinstance(actual_path, str) or not actual_path:
                proof = proof_map.get(node["ref"])
                if proof is None:
                    raise ValueError("receipt_not_recorded_parent_path_unresolved")
                used_proofs.add(node["ref"])
                actual_path = _exclusion_path(node, proof, target, by_id, trace, fingerprint, root, page_content)
            if actual_path.split(" > ")[-1] != leaf:
                raise ValueError("receipt_not_recorded_parent_path_invalid")
            if actual_path == path:
                raise ValueError("receipt_not_recorded_candidate_was_returned")
    if set(proof_map) != used_proofs:
        raise ValueError("receipt_exclusion_unused_or_wrong_node")
    return frame_index


def _completion_not_recorded(candidate, finding, receipt, by_id, trace, fingerprint, root):
    ids = receipt.get("relation_evidence_ids")
    if (candidate.get("domain") != "cpu" or candidate.get("metric") != "wait"
            or finding.get("conclusion_status") != "Unresolved" or receipt.get("purpose") != "wait_completion"
            or receipt.get("entity_json_pointers") or not isinstance(ids, list) or len(ids) != 2
            or len(set(ids)) != 2 or ids[0] != receipt["evidence_id"]):
        raise ValueError("receipt_completion_not_recorded_requires_unresolved_wait_pair")
    first = None
    for eid, direction in zip(ids, ("source_kind", "target_kind")):
        method, params, content = _receipt(root, by_id[eid], trace["trace_id"], fingerprint)
        if method != "relation.search" or set(params) - (_BUDGET_PARAMS | {direction}) or params.get(direction) != "cpu_zone":
            raise ValueError("receipt_completion_requires_unfiltered_cpu_zone_direction")
        if first is not None:
            _same_revision(first, content)
            if content["trace"] != first["trace"]:
                raise ValueError("receipt_completion_relation_snapshot_mismatch")
        else:
            first = content
        values = _complete_collection_page(content, "relations")
        data = content["data"]
        if (values or data.get("present") is not True or data.get("complete") is not True
                or data.get("provenance") != "exact-binary" or str(data.get("matched_count")) != "0"
                or "relation_count" not in data or isinstance(data["relation_count"], bool) or int(data["relation_count"]) < 0):
            raise ValueError("receipt_completion_relation_domain_not_proved_empty")
        if first["data"]["relation_count"] != data["relation_count"]:
            raise ValueError("receipt_completion_relation_count_mismatch")
    return first


def validate_receipts(candidate: dict, finding: dict, evidence: dict, root: Path) -> list[str]:
    prefix = candidate.get("candidate_id", "candidate")
    errors: list[str] = []
    by_id = {e.get("evidence_id"): e for e in evidence.get("evidence", []) if isinstance(e, dict)}
    receipts = finding.get("investigation_receipts")
    if not isinstance(receipts, list) or not receipts:
        return [f"{prefix}: no actual investigation receipts"]
    covered_frames: set[str] = set()
    covered_intervals: set[int] = set()
    purposes: set[str] = set()
    unit = candidate.get("observation_unit", "frame")
    target = candidate.get("thread_or_queue", "")
    frame_scope = candidate.get("frame_scope", "")
    path = candidate.get("structural_signature", "")
    leaf = path.split(" > ")[-1]
    def node_target(node):
        # A GPU Zone also names its CPU producer thread; that is not its Queue.
        if candidate.get("domain") == "gpu":
            return node.get("context_ref", node.get("gpu_context_ref"))
        return node.get("thread_ref")
    gaps: dict[str, dict] = {}
    trace = evidence.get("trace", {})
    fingerprint = trace.get("trace_sha256", trace.get("sha256", trace.get("fingerprint", "")))
    completion_boundary = None
    actual_tree_contents = []
    for receipt in receipts:
        try:
            item = by_id[receipt["evidence_id"]]
            unavailable = receipt.get("outcome") == "unavailable"
            if "page_evidence_ids" in receipt:
                method, params, content = _receipt_chain(root, receipt, by_id, trace.get("trace_id", ""), fingerprint)
            else:
                anchor_receipt = receipt.get("unavailable_anchor", {}) if unavailable else {}
                anchor_item = by_id.get(anchor_receipt.get("evidence_id"))
                method, params, content = _receipt(root, item, trace.get("trace_id", ""), fingerprint, unavailable,
                    identity_anchor=anchor_item)
            if unavailable and (finding.get("conclusion_status") != "Unresolved" or not content.get("_unavailable_code")):
                raise ValueError("receipt_unavailable_requires_actual_capability_failure_and_unresolved_conclusion")
            if receipt.get("manifestation") != candidate.get("manifestation", ""):
                raise ValueError("receipt_manifestation_mismatch")
            if receipt.get("outcome", "available") not in {"available", "unavailable", "verified_absent", "verified_not_recorded", "completion_not_recorded"}:
                raise ValueError("receipt_unknown_outcome")
            if receipt.get("outcome") == "verified_absent":
                covered_frames.add(_verified_empty_frame(candidate, finding, receipt, method, params,
                    content, by_id, trace, fingerprint, root))
                purposes.add("zone_tree")
                continue
            if receipt.get("outcome") == "verified_not_recorded":
                covered_frames.add(_verified_candidate_not_recorded(candidate, finding, receipt, method, params,
                    content, by_id, trace, fingerprint, root))
                purposes.add("zone_tree")
                actual_tree_contents.append(content)
                continue
            if receipt.get("outcome") == "completion_not_recorded":
                completion_boundary = _completion_not_recorded(candidate, finding, receipt, by_id, trace, fingerprint, root)
                purposes.add("wait_completion")
                continue
            purpose = receipt.get("purpose", "")
            allowed = {
                "zone_tree": ("zone.cpu.", "zone.gpu.", "frame.explain", "frame.correlated_timeline"),
                "frame_context": ("frame.",), "sampling": ("sample.", "context_switch."),
                "wait_completion": ("job.", "relation.", "gfx.", "zone.cpu.wait"),
                "source_lookup": ("source.", "symbol.", "script.", "callstack."),
                "resource": ("gpu.", "memory."), "io": ("io.",),
                "counterevidence": ("zone.", "frame.", "sample.", "job.", "gpu.", "memory.", "io."),
            }
            if purpose not in allowed or not method.startswith(allowed[purpose]):
                raise ValueError("receipt_purpose_method_mismatch")
            if unavailable:
                anchor_receipt = receipt.get("unavailable_anchor")
                if (not isinstance(anchor_receipt, dict) or anchor_receipt.get("unavailable_anchor")
                        or anchor_receipt.get("outcome", "available") != "available"):
                    raise ValueError("receipt_unavailable_requires_verified_entity_anchor")
                anchor_item = by_id[anchor_receipt["evidence_id"]]
                _, _, anchor_content = _receipt(root, anchor_item, trace["trace_id"], fingerprint)
                pointers = anchor_receipt.get("entity_json_pointers", [])
                ref_pointer = receipt["unavailable_ref_json_pointer"]
                directed_purposes = {"zone_tree"} if candidate.get("domain", "cpu") in {"cpu", "gpu"} else {"wait_completion", "resource", "io"}
                if (anchor_receipt.get("purpose") not in directed_purposes or len(pointers) != 1
                        or ref_pointer not in {pointers[0] + "/" + key for key in
                            ("ref", "source_location_ref", "source_ref", "callstack_ref", "symbol_ref")}):
                    raise ValueError("receipt_unavailable_requires_directed_entity_reference")
                ref = _pointer(anchor_content, ref_pointer)
                if not isinstance(ref, str) or not ref or params.get("ref") != ref:
                    raise ValueError("receipt_unavailable_ref_does_not_match_anchor")
                # The anchor must itself complete directed coverage for this
                # candidate. A source error cannot stand in for a missing tree.
                other_receipts = [r for r in receipts if isinstance(r, dict) and r.get("outcome", "available") == "available"
                    and r.get("evidence_id") != anchor_receipt["evidence_id"]]
                anchor_finding = dict(finding, investigation_receipts=[anchor_receipt, *other_receipts])
                anchor_errors = validate_receipts(candidate, anchor_finding, evidence, root)
                if anchor_errors:
                    raise ValueError("receipt_unavailable_anchor_invalid: " + "; ".join(anchor_errors))
                key = json.dumps([method, params, purpose, content["_unavailable_code"]], sort_keys=True)
                gap = gaps.setdefault(key, {"ids": set(), "frames": set(), "intervals": set(), "purpose": purpose})
                gap["ids"].add(content["_request_id"])
                # All candidate representatives were independently verified by
                # the anchor above, not inferred from ignored request fields.
                gap["frames"].update(str(f) for f in candidate.get("representative_frames", []))
                gap["intervals"].update(range(len(candidate.get("representative_intervals", []))))
                purposes.add(anchor_receipt["purpose"])
                continue
            previous_frames, previous_intervals = covered_frames.copy(), covered_intervals.copy()
            nodes = [_pointer(content, p) for p in receipt.get("entity_json_pointers", [])]
            matched_nodes = []
            proved_paths = {}
            if target and any(params.get(key) is not None and params[key] != target
                              for key in ("thread_ref", "context_ref", "gpu_context_ref")):
                raise ValueError("receipt_explicit_thread_or_queue_mismatch")
            scoped = target in {params.get("thread_ref"), params.get("context_ref"), params.get("gpu_context_ref")}
            if target and not scoped and not any(isinstance(n, dict) and target in
                {n.get("thread_ref"), n.get("context_ref"), n.get("gpu_context_ref")} for n in nodes):
                raise ValueError("receipt_thread_or_queue_mismatch")
            if purpose == "zone_tree" and not unavailable:
                for node in nodes:
                    if not isinstance(node, dict):
                        continue
                    actual_target = node_target(node)
                    if target and actual_target is not None and actual_target != target:
                        continue
                    if candidate.get("frame_root") or (frame_scope and candidate.get("signature_id") == "frame-wall:" + frame_scope):
                        # A FrameSet duration is not a Zone with that name.
                        # Require an actual complete root of the queried tree;
                        # frame scope/range is validated separately below.
                        if "parent_ref" in node and node["parent_ref"] is None and node.get("complete") is True:
                            matched_nodes.append(node)
                        continue
                    name = node.get("name", node.get("label", node.get("function")))
                    if name != leaf:
                        continue
                    matched = False
                    chain = []
                    if node.get("path") == path or node.get("signature_id") == candidate.get("signature_id"):
                        matched = True
                    elif receipt.get("parent_chain_json_pointers"):
                        chain = [_pointer(content, p) for p in receipt["parent_chain_json_pointers"]]
                        linked = chain + [node]
                        links_match = all(child.get("parent_ref") is not None and child["parent_ref"] == parent.get("ref")
                                          for parent, child in zip(linked, linked[1:]))
                        matched = links_match and " > ".join([parent.get("name", "") for parent in chain] + [name]) == path
                    elif receipt.get("parent_chain_evidence"):
                        chain = []
                        for parent_item in receipt["parent_chain_evidence"]:
                            pm, pp, pc = _receipt(root, by_id[parent_item["evidence_id"]], trace["trace_id"], fingerprint)
                            parent = _pointer(pc, parent_item.get("entity_json_pointer", "/data"))
                            expected_method = "zone.gpu.get" if candidate.get("domain") == "gpu" else "zone.cpu.get"
                            if (pm != expected_method or not isinstance(parent, dict)
                                    or pp.get("ref") != parent.get("ref") or not pp.get("ref")):
                                raise ValueError("receipt_parent_get_ref_mismatch")
                            parent_target = node_target(parent)
                            if target and parent_target != target:
                                raise ValueError("receipt_parent_thread_or_queue_mismatch")
                            chain.append(parent)
                        linked = chain + [node]
                        refs = [n.get("ref") for n in linked]
                        links_match = len(set(refs)) == len(refs) and all(
                            child.get("parent_ref") is not None and child["parent_ref"] == parent.get("ref")
                            for parent, child in zip(linked, linked[1:]))
                        matched = links_match and " > ".join([p.get("name", "") for p in chain] + [name]) == path
                    elif " > " not in path:
                        matched = True
                    if matched:
                        matched_nodes.append(node)
                        proved_paths[node.get("ref")] = chain + [node]
                if not matched_nodes:
                    raise ValueError("receipt_entity_or_parent_path_mismatch")
                actual_tree_contents.append(content)
            if not unavailable:
                purposes.add(purpose)
            frame_index = params.get("index", params.get("frame_index"))
            request_scope = params.get("frame_scope", params.get("frame_set_ref", params.get("frame_set")))
            directed = purpose == "zone_tree" if candidate.get("domain", "cpu") in {"cpu", "gpu"} else purpose in {"resource", "io", "wait_completion"}
            job_directed = candidate.get("domain") == "job" and directed
            if job_directed and not unavailable:
                # Job representatives are recorded OriginFrameIds, not indexes
                # into Player.Frame. Neither a name nor invented index params
                # can prove that this Job belongs to the requested frame.
                if purpose != "wait_completion" or method not in {"job.get", "job.dependencies"}:
                    raise ValueError("receipt_job_requires_directed_details")
                job = content["data"] if method == "job.get" else content["data"]["job"]
                if (not job.get("ref") or params.get("ref") != job["ref"]
                        or not any(node == job for node in nodes) or job.get("truncated") is True):
                    raise ValueError("receipt_job_identity_mismatch")
                if (job.get("name") != path or candidate.get("signature_id") !=
                        "job:type:" + str(job.get("type_id")) + ":" + job.get("name", "")):
                    raise ValueError("receipt_job_signature_mismatch")
                amethod, aparams, anchor = _receipt(root, by_id[receipt["job_frame_identity_evidence_id"]],
                    trace["trace_id"], fingerprint)
                identity = anchor["data"]["identity"]
                expected_domain = {"Player.Frame": "player", "Editor.Frame": "editor", "Render.Frame": "render"}.get(frame_scope)
                if (amethod != "frame.identity" or anchor["data"].get("present") is not True
                        or not expected_domain or identity.get("complete") is not True
                        or identity.get("evidence_kind") != "exact"
                        or str(aparams.get("frame_id")) != str(identity.get("frame_id"))
                        or job.get("origin_frame_ref") != identity.get("ref")
                        or job.get("origin_frame_sequence") != identity.get("sequence")
                        or int(identity["begin_ns"]) >= int(identity["end_ns"])):
                    raise ValueError("receipt_job_origin_frame_mismatch")
                phases = set()
                for event in identity.get("events", []):
                    phase = event.get("phase")
                    if (event.get("canonical") is True and event.get("alias") is False
                            and event.get("domain") == expected_domain and phase in {"begin", "end"}
                            and event.get("frame_ref") == identity["ref"]
                            and str(event.get("frame_id")) == str(identity["frame_id"])
                            and event.get("time_ns") == identity[phase + "_ns"]):
                        phases.add(phase)
                if phases != {"begin", "end"}:
                    raise ValueError("receipt_job_origin_missing_canonical_boundary")
                covered_frames.add(str(identity["frame_id"]))
            # Ref-based methods ignore caller-supplied frame/time selectors.
            # Only their returned entity interval plus a real frame/L0 anchor
            # can provide coverage; ignored fields must never add another frame.
            if (unit == "frame" and directed and not job_directed and not params.get("ref")
                    and frame_index is not None and (not frame_scope or request_scope == frame_scope)):
                covered_frames.add(str(frame_index))
            # A time query needs a verified frame.get receipt to prove which
            # FrameSet/index it covers; declared Evidence refs are not sufficient.
            begin, end = params.get("start_ns", params.get("begin_ns")), params.get("end_ns")
            if unit == "frame" and directed and not job_directed and receipt.get("frame_anchor_evidence_id"):
                amethod, aparams, anchor = _receipt(root, by_id[receipt["frame_anchor_evidence_id"]], trace["trace_id"], fingerprint)
                if amethod != "frame.get":
                    raise ValueError("receipt_frame_anchor_not_frame_get")
                frame = _pointer(anchor, receipt.get("frame_anchor_json_pointer", "/data"))
                anchor_scope = frame.get("frame_set_ref", frame.get("frame_scope"))
                anchor_index = frame.get("index", frame.get("frame_index", aparams.get("index")))
                frame_begin, frame_end = int(frame["begin_ns"]), int(frame["end_ns"])
                if (str(aparams.get("index")) != str(anchor_index)
                        and not (aparams.get("ref") and aparams["ref"] == frame.get("ref"))):
                    raise ValueError("receipt_frame_anchor_request_response_mismatch")
                range_covers = (not params.get("ref") and begin is not None and end is not None
                                and int(begin) <= frame_begin and int(end) >= frame_end)
                # The real tree API is ref-based. Do not invent range params:
                # use the returned matching event's half-open interval instead.
                ref_covers = False
                if method in {"zone.cpu.tree", "zone.cpu.get"} and params.get("ref"):
                    for node in matched_nodes:
                        if node.get("ref") != params["ref"] or node.get("timing_valid") is False:
                            continue
                        node_begin, node_end = node.get("start_ns"), node.get("end_ns")
                        if node_begin is not None and node_end is not None:
                            ref_covers = int(node_begin) < frame_end and int(node_end) > frame_begin
                            if ref_covers:
                                break
                if anchor_scope != frame_scope or frame_begin >= frame_end or not (range_covers or ref_covers):
                    raise ValueError("receipt_frame_anchor_scope_mismatch")
                covered_frames.add(str(anchor_index))
            if unit == "l0_segment" and directed and not params.get("ref") and begin is not None and end is not None:
                for i, interval in enumerate(candidate.get("representative_intervals", [])):
                    if int(begin) <= int(interval["begin_ns"]) and int(end) >= int(interval["end_ns"]):
                        covered_intervals.add(i)
            if unit == "l0_segment" and directed and not unavailable and receipt.get("interval_anchor_evidence_id"):
                amethod, aparams, anchor = _receipt(root, by_id[receipt["interval_anchor_evidence_id"]], trace["trace_id"], fingerprint)
                if method != "zone.gpu.tree" or amethod != "zone.gpu.get":
                    raise ValueError("receipt_gpu_anchor_method_mismatch")
                segment = _pointer(anchor, receipt.get("interval_anchor_json_pointer", "/data"))
                segment_ref = segment.get("ref")
                if (not segment_ref or aparams.get("ref") != segment_ref or node_target(segment) != target
                        or segment.get("parent_ref") is not None or segment.get("complete") is not True):
                    raise ValueError("receipt_gpu_anchor_identity_mismatch")
                segment_begin, segment_end = int(segment["gpu_start_ns"]), int(segment["gpu_end_ns"])
                if segment_begin >= segment_end:
                    raise ValueError("receipt_gpu_anchor_invalid_time")
                for node in matched_nodes:
                    if params.get("ref") != node.get("ref") or node.get("complete") is not True:
                        continue
                    # An equal name or an overlapping time interval is not ancestry.
                    if segment_ref not in {n.get("ref") for n in proved_paths.get(node.get("ref"), [])}:
                        continue
                    node_begin, node_end = int(node["gpu_start_ns"]), int(node["gpu_end_ns"])
                    if not segment_begin <= node_begin <= node_end <= segment_end:
                        continue
                    for i, interval in enumerate(candidate.get("representative_intervals", [])):
                        if (segment_ref in interval.get("event_refs", [])
                                and segment_begin == int(interval["begin_ns"])
                                and segment_end == int(interval["end_ns"])):
                            covered_intervals.add(i)
            if unavailable:
                key = json.dumps([method, params, purpose, content["_unavailable_code"]], sort_keys=True)
                gap = gaps.setdefault(key, {"ids": set(), "frames": set(), "intervals": set(), "purpose": purpose})
                gap["ids"].add(content["_request_id"])
                gap["frames"].update(covered_frames - previous_frames)
                gap["intervals"].update(covered_intervals - previous_intervals)
                covered_frames, covered_intervals = previous_frames, previous_intervals
        except (KeyError, TypeError, ValueError, OSError, IndexError) as exc:
            errors.append(f"{prefix}: {exc}")
    for gap in gaps.values():
        if len(gap["ids"]) < 3:
            errors.append(f"{prefix}: unavailable requires three distinct actual scoped attempts")
        else:
            covered_frames.update(gap["frames"])
            covered_intervals.update(gap["intervals"])
            purposes.add(gap["purpose"])
    if completion_boundary is not None:
        for tree_content in actual_tree_contents:
            try:
                _same_revision(tree_content, completion_boundary)
            except ValueError as exc:
                errors.append(f"{prefix}: {exc}")
    if candidate.get("domain", "cpu") in {"cpu", "gpu"} and "zone_tree" not in purposes:
        errors.append(f"{prefix}: no actual tree/limiter investigation")
    if candidate.get("metric") == "wait" and "wait_completion" not in purposes:
        errors.append(f"{prefix}: wait completion end was not investigated")
    if unit == "frame":
        missing = set(map(str, candidate.get("representative_frames", []))) - covered_frames
        if missing:
            errors.append(f"{prefix}: unqueried representative frames {sorted(missing)}")
    elif unit == "l0_segment":
        intervals = candidate.get("representative_intervals", [])
        if not intervals or len(covered_intervals) != len(intervals):
            errors.append(f"{prefix}: unqueried GPU time intervals (segment ordinals are not frame IDs)")
    return errors
