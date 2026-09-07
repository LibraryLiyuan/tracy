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


def _receipt(root: Path, item: dict, trace_id: str, fingerprint: str, allow_unavailable=False) -> tuple[str, dict, dict]:
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
    if len(fingerprint) != 64 or structured.get("trace", {}).get("fingerprint", "").lower() != fingerprint.lower():
        raise ValueError("receipt_source_fingerprint_mismatch")
    if response.get("error") or response.get("result", {}).get("isError") or not structured.get("ok"):
        error = structured.get("error", response.get("error", {}))
        code = error.get("code", "") if isinstance(error, dict) else ""
        # Invalid arguments, wrong IDs, budget exhaustion and arbitrary errors
        # are not evidence that the source capability is absent.
        if not allow_unavailable or code not in {
            "capability_unavailable", "domain_unavailable", "not_captured",
            "unsupported_backend", "source_domain_invalid", "insufficient_evidence",
        }:
            raise ValueError("receipt_query_failed_actual_error:" + json.dumps(error))
        structured = dict(structured, _unavailable_code=code, _request_id=request["id"])
    incomplete = structured.get("partial") or structured.get("truncated")
    for key in ("page", "pagination"):
        page = structured.get(key)
        if page is not None:
            if not isinstance(page, dict):
                raise ValueError("receipt_invalid_pagination")
            incomplete = incomplete or page.get("partial") or page.get("truncated") or page.get("next_cursor")
    if incomplete:
        raise ValueError("receipt_query_incomplete")
    return args["method"], args.get("params", {}), structured


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
    gaps: dict[str, dict] = {}
    trace = evidence.get("trace", {})
    fingerprint = trace.get("trace_sha256", trace.get("sha256", trace.get("fingerprint", "")))
    for receipt in receipts:
        try:
            item = by_id[receipt["evidence_id"]]
            unavailable = receipt.get("outcome") == "unavailable"
            method, params, content = _receipt(root, item, trace.get("trace_id", ""), fingerprint, unavailable)
            if unavailable and (finding.get("conclusion_status") != "Unresolved" or not content.get("_unavailable_code")):
                raise ValueError("receipt_unavailable_requires_actual_capability_failure_and_unresolved_conclusion")
            if receipt.get("manifestation") != candidate.get("manifestation", ""):
                raise ValueError("receipt_manifestation_mismatch")
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
            previous_frames, previous_intervals = covered_frames.copy(), covered_intervals.copy()
            if unavailable and params.get("signature_id") != candidate.get("signature_id"):
                raise ValueError("receipt_unavailable_does_not_target_signature")
            nodes = [_pointer(content, p) for p in receipt.get("entity_json_pointers", [])]
            matched_nodes = []
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
                    node_target = node.get("thread_ref", node.get("gpu_context_ref", node.get("context_ref")))
                    if target and node_target is not None and node_target != target:
                        continue
                    if candidate.get("frame_root"):
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
                    if node.get("path") == path or node.get("signature_id") == candidate.get("signature_id"):
                        matched = True
                    elif receipt.get("parent_chain_json_pointers"):
                        chain = [_pointer(content, p) for p in receipt["parent_chain_json_pointers"]]
                        linked = chain + [node]
                        links_match = all(child.get("parent_ref") is not None and child["parent_ref"] == parent.get("ref")
                                          for parent, child in zip(linked, linked[1:]))
                        matched = links_match and " > ".join([parent.get("name", "") for parent in chain] + [name]) == path
                    elif " > " not in path:
                        matched = True
                    if matched:
                        matched_nodes.append(node)
                if not matched_nodes:
                    raise ValueError("receipt_entity_or_parent_path_mismatch")
            if not unavailable:
                purposes.add(purpose)
            frame_index = params.get("index", params.get("frame_index"))
            request_scope = params.get("frame_scope", params.get("frame_set_ref", params.get("frame_set")))
            directed = purpose == "zone_tree" if candidate.get("domain", "cpu") in {"cpu", "gpu"} else purpose in {"resource", "io", "wait_completion"}
            if unit == "frame" and directed and frame_index is not None and (not frame_scope or request_scope == frame_scope):
                covered_frames.add(str(frame_index))
            # A time query needs a verified frame.get receipt to prove which
            # FrameSet/index it covers; declared Evidence refs are not sufficient.
            begin, end = params.get("start_ns", params.get("begin_ns")), params.get("end_ns")
            if unit == "frame" and directed and receipt.get("frame_anchor_evidence_id"):
                amethod, aparams, anchor = _receipt(root, by_id[receipt["frame_anchor_evidence_id"]], trace["trace_id"], fingerprint)
                if amethod != "frame.get":
                    raise ValueError("receipt_frame_anchor_not_frame_get")
                frame = _pointer(anchor, receipt.get("frame_anchor_json_pointer", "/data"))
                anchor_scope = frame.get("frame_set_ref", frame.get("frame_scope"))
                anchor_index = frame.get("index", frame.get("frame_index", aparams.get("index")))
                frame_begin, frame_end = int(frame["begin_ns"]), int(frame["end_ns"])
                range_covers = begin is not None and end is not None and int(begin) <= frame_begin and int(end) >= frame_end
                # The real tree API is ref-based. Do not invent range params:
                # use the returned matching event's half-open interval instead.
                ref_covers = False
                if method == "zone.cpu.tree" and params.get("ref"):
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
            if unit == "l0_segment" and directed and begin is not None and end is not None:
                for i, interval in enumerate(candidate.get("representative_intervals", [])):
                    if int(begin) <= int(interval["begin_ns"]) and int(end) >= int(interval["end_ns"]):
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
