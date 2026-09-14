#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Validation and deterministic specialty rendering for Query-native reports."""

from __future__ import annotations

import re
from typing import Any

from candidate_manifest import is_candidate_collection
from review_routing import active as routing_active, validate_routing, query_required
from report_common import render_minimum_evidence


SPECIALTY_REPORT_FILES = [
    "01-Trace-Quality-and-Scan-Coverage.md",
    "02-Frame-and-CPU-Analysis.md",
    "03-GPU-and-Render-Analysis.md",
    "04-Job-and-Scheduling-Analysis.md",
    "05-Memory-and-GPU-Resource-Analysis.md",
    "06-IO-Sampling-and-System-Analysis.md",
    "07-Tracy-Telemetry-Overhead.md",
    "08-Profiler-Manual-Verification-Guide.md",
    "09-Investigation-Backlog-and-Evidence-Gaps.md",
]

TRAIL_STEPS = [
    "discovery",
    "statistics",
    "representative_frames",
    "limiter_analysis",
    "hypotheses",
    "source_analysis",
    "counterevidence",
    "conclusion",
]

CONFIRMATION_BASES = {"typed_relation", "source_revision_match", "independent_evidence"}


def is_query_native(analysis: dict[str, Any]) -> bool:
    versions = analysis.get("report_versions")
    return isinstance(analysis.get("query_scan"), dict) or (
        isinstance(versions, dict) and versions.get("analysis_workflow") in {"2.0.0", "2.1.0", "2.2.0"}
    )


def _candidate_required(candidate: dict[str, Any]) -> bool:
    return (candidate.get("selected") is True or candidate.get("priority") in {"P0", "P1"}
            or "user_focus" in candidate.get("triggers", []))


def validate_query_native_analysis(
    analysis: dict[str, Any], candidate_manifest: dict[str, Any], evidence_manifest=None, evidence_root=None
) -> list[str]:
    errors: list[str] = []
    if not is_query_native(analysis):
        return ["Query-native validation requires analysis.query_scan and workflow 2.0 identity"]
    if candidate_manifest.get("schema_version") != 1:
        errors.append("candidate_manifest.schema_version must be 1")
    if candidate_manifest.get("policy_algorithm") != "candidate-policy-v3":
        return [*errors, "candidate_manifest.policy_algorithm must be candidate-policy-v3"]
    candidates = candidate_manifest.get("candidates")
    if not is_candidate_collection(candidates):
        return [*errors, "candidate_manifest.candidates must be an array"]
    # Keep detailed numeric facts only for actual findings. All IDs and all
    # mandatory entries are still audited, including late/unselected records.
    raw_findings = analysis.get("candidate_findings")
    finding_ids = {item.get("candidate_id") for item in (raw_findings if isinstance(raw_findings, list) else [])
                   if isinstance(item, dict) and isinstance(item.get("candidate_id"), str)}
    by_id: dict[str, dict[str, Any]] = {}
    all_ids: set[str] = set()
    required: set[str] = set()
    routing = None
    if routing_active(analysis):
        try:
            routing, _ = validate_routing(analysis, candidate_manifest, evidence_manifest, evidence_root)
        except (KeyError, TypeError, ValueError, OSError) as exc:
            return [*errors, "review routing: " + str(exc)]
    for index, candidate in enumerate(candidates):
        if not isinstance(candidate, dict) or not isinstance(candidate.get("candidate_id"), str):
            errors.append(f"candidate_manifest.candidates[{index}] is invalid")
            continue
        candidate_id = candidate["candidate_id"]
        if candidate.get("domain") == "quality" or "quality" in candidate.get("triggers", []):
            errors.append("legacy quality candidate requires candidate-policy-v3; capture quality belongs in appendix")
        if candidate_id in all_ids:
            errors.append(f"candidate_manifest contains duplicate candidate {candidate_id}")
        all_ids.add(candidate_id)
        if (query_required(candidate, routing) if routing is not None else _candidate_required(candidate)):
            required.add(candidate_id)
        if candidate_id in finding_ids:
            by_id[candidate_id] = candidate

    scan = analysis.get("query_scan")
    capture_quality = candidate_manifest.get("capture_quality", [])
    if not isinstance(capture_quality, list) or any(
        not isinstance(item, dict) or any(key not in item for key in ("domain", "status", "reason"))
        or "priority" in item or "selected" in item for item in capture_quality
    ):
        errors.append("candidate_manifest.capture_quality must contain unranked domain limitations")
    if analysis.get("capture_quality", []) != capture_quality:
        errors.append("analysis.capture_quality differs from Candidate Manifest; preserve appendix evidence")
    if not isinstance(scan, dict):
        errors.append("analysis.query_scan is required for workflow 2.0.0")
        scan = {}
    if scan.get("aggregate_content_sha256") != candidate_manifest.get("aggregate_content_sha256"):
        errors.append("analysis.query_scan.aggregate_content_sha256 does not match Candidate Manifest")
    if scan.get("candidate_content_sha256") != candidate_manifest.get("content_sha256"):
        errors.append("analysis.query_scan.candidate_content_sha256 does not match Candidate Manifest")

    findings = analysis.get("candidate_findings")
    if not isinstance(findings, list):
        errors.append("analysis.candidate_findings must be an array")
        findings = []
    finding_by_id: dict[str, dict[str, Any]] = {}
    signature_ids: set[str] = set()
    for index, finding in enumerate(findings):
        path = f"analysis.candidate_findings[{index}]"
        if not isinstance(finding, dict):
            errors.append(f"{path} must be an object")
            continue
        candidate_id = finding.get("candidate_id")
        if not isinstance(candidate_id, str) or candidate_id not in by_id:
            errors.append(f"{path}.candidate_id does not reference Candidate Manifest")
            continue
        if candidate_id in finding_by_id:
            errors.append(f"{path}.candidate_id is duplicated")
        finding_by_id[candidate_id] = finding
        candidate = by_id[candidate_id]
        signature_id = finding.get("signature_id")
        if signature_id != candidate.get("signature_id"):
            errors.append(f"{path}.signature_id does not match Candidate Manifest")
        if isinstance(signature_id, str):
            signature_ids.add(signature_id)
        if finding.get("priority") != candidate.get("priority"):
            errors.append(f"{path}.priority does not match Candidate Manifest")
        if finding.get("query_numeric_facts") != candidate.get("trigger_evidence", []):
            errors.append(f"{path}.query_numeric_facts differs from Candidate Manifest")
        if finding.get("representative_frames") != candidate.get("representative_frame_details", []):
            errors.append(f"{path}.representative_frames differs from Candidate Manifest")
        if candidate_manifest.get("policy_algorithm") == "candidate-policy-v3":
            if finding.get("representative_intervals", []) != candidate.get("representative_intervals", []):
                errors.append(f"{path}.representative_intervals differs from Candidate Manifest")
            if not finding.get("investigation_receipts"):
                errors.append(f"{path}.investigation_receipts required; prose alone is not investigation")
            elif evidence_manifest is None or evidence_root is None:
                errors.append(f"{path}.receipt validation requires original MCP artifacts")
            else:
                from investigation_receipts import validate_receipts
                errors.extend(validate_receipts(candidate, finding, evidence_manifest, evidence_root))
            for receipt in finding.get("investigation_receipts", []):
                outcome = receipt.get("outcome")
                reasons = {
                    "verified_absent": "NoRecordedCpuZonesInRepresentativeFrame",
                    "verified_not_recorded": "CandidateNotRecordedInRepresentativeFrame",
                    "completion_not_recorded": "UnresolvedWaitChain",
                }
                if outcome not in reasons:
                    continue
                # An absence receipt closes only the evidence investigation;
                # neither absent events nor unrecorded completion links imply
                # zero cost or a confirmed cause. Every dependency stays visible.
                required_refs = {receipt.get("evidence_id")}
                required_refs.update(receipt.get("page_evidence_ids", []))
                if outcome == "completion_not_recorded":
                    required_refs.update(receipt.get("relation_evidence_ids", []))
                else:
                    required_refs.add(receipt.get("frame_anchor_evidence_id"))
                    if receipt.get("frame_set_evidence_id"):
                        required_refs.add(receipt["frame_set_evidence_id"])
                    for exclusion in receipt.get("exclusion_evidence", []):
                        required_refs.update(item.get("evidence_id") for item in exclusion.get("parent_chain_evidence", []))
                if not any(isinstance(gap, dict) and gap.get("candidate_id") == candidate_id
                        and gap.get("reason") == reasons[outcome]
                        and isinstance(gap.get("evidence_refs"), list)
                        and required_refs <= set(gap["evidence_refs"])
                        and isinstance(gap.get("blocked_by"), str) and gap["blocked_by"].strip()
                        and isinstance(gap.get("minimum_additional_evidence"), list)
                        and gap["minimum_additional_evidence"]
                        and all(isinstance(value, str) and value.strip() for value in gap["minimum_additional_evidence"])
                        for gap in analysis.get("evidence_gaps", [])):
                    category = "completion_gap" if outcome == "completion_not_recorded" else "absence_gap"
                    errors.append(f"{path}.{category} missing candidate-linked evidence and minimum follow-up")
        if not isinstance(finding.get("representative_selection_reason"), str) or not finding["representative_selection_reason"].strip():
            errors.append(f"{path}.representative_selection_reason is required")

        trail = finding.get("analysis_trail")
        if not isinstance(trail, dict):
            errors.append(f"{path}.analysis_trail is required")
        else:
            for step in TRAIL_STEPS:
                if not isinstance(trail.get(step), str) or not trail[step].strip():
                    errors.append(f"{path}.analysis_trail.{step} is required")

        conclusion = finding.get("conclusion_status")
        if conclusion not in {"Confirmed", "Supported", "Hypothesis", "Unresolved", "Rejected"}:
            errors.append(f"{path}.conclusion_status is invalid")
        bases = finding.get("confirmation_basis")
        if not isinstance(bases, list) or any(item not in CONFIRMATION_BASES for item in bases):
            errors.append(f"{path}.confirmation_basis is invalid")
            bases = []
        if conclusion == "Confirmed" and not bases:
            errors.append(f"{path}.confirmation_basis is required for Confirmed")

        source_evidence = finding.get("source_evidence")
        if not isinstance(source_evidence, list):
            errors.append(f"{path}.source_evidence must be an array")
            source_evidence = []
        for source_index, source in enumerate(source_evidence):
            if not isinstance(source, dict):
                errors.append(f"{path}.source_evidence[{source_index}] must be an object")
                continue
            if source.get("used_for_confirmation") is True and source.get("revision_match") is not True:
                errors.append(f"{path}.source_evidence[{source_index}] revision mismatch cannot support Confirmed")
        if conclusion == "Confirmed" and "source_revision_match" in bases:
            if not any(
                isinstance(source, dict)
                and source.get("used_for_confirmation") is True
                and source.get("revision_match") is True
                for source in source_evidence
            ):
                errors.append(f"{path}.confirmation_basis source_revision_match lacks matching source evidence")

        from confirmation_evidence import validate_confirmation_evidence
        errors.extend(validate_confirmation_evidence(finding, evidence_manifest, evidence_root, path))

        evidence_refs = finding.get("evidence_refs")
        if not isinstance(evidence_refs, list) or not evidence_refs:
            errors.append(f"{path}.evidence_refs must not be empty")
        if conclusion == "Confirmed" and "independent_evidence" in bases and len(evidence_refs or []) < 2:
            errors.append(f"{path}.confirmation_basis independent_evidence requires at least two Evidence refs")
        if conclusion == "Confirmed" and "typed_relation" in bases:
            signature = next(
                (item for item in analysis.get("bottleneck_signatures", []) if item.get("id") == signature_id),
                None,
            )
            if not isinstance(signature, dict) or not signature.get("causal_chain"):
                errors.append(f"{path}.confirmation_basis typed_relation lacks a validated causal chain")

    missing_required = sorted(required - set(finding_by_id))
    status = analysis.get("report_status", "complete")
    if status not in {"complete", "in_progress"}:
        errors.append("analysis.report_status must be complete or in_progress")
    if status != "in_progress":
        for candidate_id in missing_required:
            errors.append(f"required candidate {candidate_id} has no completed investigation")

    report_signatures = {
        item.get("id") for item in analysis.get("bottleneck_signatures", []) if isinstance(item, dict)
    }
    for signature_id in sorted(report_signatures - signature_ids):
        errors.append(f"bottleneck signature {signature_id} has no Candidate trigger path")

    backlog = analysis.get("investigation_backlog")
    if not isinstance(backlog, list):
        errors.append("analysis.investigation_backlog must be an array")
        backlog = []
    backlog_ids = {
        item.get("candidate_id") for item in backlog if isinstance(item, dict)
    }
    expected_backlog = all_ids - set(finding_by_id)
    if backlog_ids != expected_backlog:
        errors.append("analysis.investigation_backlog does not enumerate every uninvestigated candidate")

    coverage = scan.get("coverage")
    if not isinstance(coverage, dict):
        errors.append("analysis.query_scan.coverage is required")
    else:
        expected = {
            "candidate_total": len(all_ids),
            "required_total": len(required),
            "required_completed": len(required & set(finding_by_id)),
            "investigated_total": len(finding_by_id),
            "uninvestigated_total": len(expected_backlog),
        }
        for key, value in expected.items():
            if coverage.get(key) != value:
                errors.append(f"analysis.query_scan.coverage.{key} must be {value}")
    return errors


def _md(value: Any) -> str:
    return (
        str(value if value is not None else "")
        .replace("\\", "\\\\")
        .replace("|", "\\|")
        .replace("\r", " ")
        .replace("\n", " ")
    )


def _findings_for(analysis: dict[str, Any], domains: set[str]) -> list[dict[str, Any]]:
    candidates = analysis.get("candidate_findings", [])
    return [
        item
        for item in candidates
        if isinstance(item, dict)
        and any(token in str(item.get("signature_id", "")).lower() or token in str(item.get("candidate_id", "")).lower()
                for token in domains)
    ]


def _finding_sections(analysis: dict[str, Any], predicate: Any = None) -> list[str]:
    signatures = {
        item.get("id"): item for item in analysis.get("bottleneck_signatures", []) if isinstance(item, dict)
    }
    lines: list[str] = []
    for finding in analysis.get("candidate_findings", []):
        if not isinstance(finding, dict):
            continue
        signature = signatures.get(finding.get("signature_id"), {})
        classification_signature = signature or {"stable_key": finding.get("signature_id", "")}
        if predicate is not None and not predicate(finding, classification_signature):
            continue
        lines.extend(
            [
                f"## {_md(finding.get('candidate_id'))} — {_md(signature.get('stable_key', finding.get('signature_id')))}",
                "",
                f"- 优先级：`{_md(finding.get('priority'))}`",
                f"- 结论状态：`{_md(finding.get('conclusion_status'))}`",
                f"- Primary Limiter：`{_md(signature.get('primary_limiter', 'Unknown'))}`",
                f"- Evidence：{_md(', '.join(finding.get('evidence_refs', [])))}",
                "",
                "### Query 数值事实",
                "",
            ]
        )
        for fact in finding.get("query_numeric_facts", []):
            lines.append(
                f"- `{_md(fact.get('metric'))}` = `{_md(fact.get('observed_value'))} {_md(fact.get('unit'))}`；"
                f"阈值 `{_md(fact.get('threshold_value'))}`；scope `{_md(fact.get('scope'))}`；"
                f"authority `{_md(fact.get('authority'))}`。"
            )
        lines.extend(["", "### 分析过程", ""])
        trail = finding.get("analysis_trail", {})
        labels = {
            "discovery": "发现",
            "statistics": "统计",
            "representative_frames": "代表帧",
            "limiter_analysis": "逐层归因",
            "hypotheses": "假设验证",
            "source_analysis": "源码核查",
            "counterevidence": "反证",
            "conclusion": "结论",
        }
        for step in TRAIL_STEPS:
            lines.append(f"- **{labels[step]}**：{_md(trail.get(step))}")
        limited = [receipt for receipt in finding.get("investigation_receipts", [])
                   if receipt.get("outcome") in {"verified_absent", "verified_not_recorded", "completion_not_recorded", "unavailable"}]
        if limited:
            lines.extend(["", "### 调查结束的证据边界", "", "已调查至可证明边界，不代表根因已确认；缺失不等于零开销。", ""])
            for receipt in limited:
                refs = [receipt.get("evidence_id", "")]
                refs.extend(receipt.get("page_evidence_ids", []))
                refs.extend(receipt.get("relation_evidence_ids", []))
                if receipt.get("frame_anchor_evidence_id"):
                    refs.append(receipt["frame_anchor_evidence_id"])
                lines.append(f"- `{_md(receipt.get('outcome'))}`；原始 Evidence / Frame 锚点：{_md(', '.join(dict.fromkeys(refs)))}。")
        lines.append("")
    if not lines:
        lines.extend(["本专项没有已完成的 Candidate 调查。", ""])
    return lines


def render_evidence_gaps(analysis: dict[str, Any]) -> list[str]:
    """Keep the candidate, blocker and actual receipt IDs beside each limit."""
    lines = []
    for gap in analysis.get("evidence_gaps", []):
        lines.extend([
            f"- `{_md(gap.get('reason'))}`；Candidate：`{_md(gap.get('candidate_id', '域级限制'))}`。",
            "",
            f"  - 当前限制：{_md(gap.get('blocked_by', '详见对应调查记录'))}",
            f"  - Evidence：{_md(', '.join(gap.get('evidence_refs', []))) or '详见对应调查记录'}",
            f"  - 最小补证：{render_minimum_evidence(gap.get('minimum_additional_evidence', []))}",
            "",
        ])
    return lines or ["- 无。"]


def render_analysis_process(analysis: dict[str, Any]) -> str:
    if not is_query_native(analysis):
        return ""
    lines = ["## 2.1 Query 原生候选与分析过程", ""]
    scan = analysis.get("query_scan", {})
    coverage = scan.get("coverage", {})
    lines.extend(
        [
            f"- Scan：`{_md(scan.get('scan_id'))}`",
            f"- Aggregate：`{_md(scan.get('aggregate_content_sha256'))}`",
            f"- Candidate：`{_md(scan.get('candidate_content_sha256'))}`",
            f"- 覆盖：候选 {_md(coverage.get('candidate_total'))}；强制 {_md(coverage.get('required_completed'))}/{_md(coverage.get('required_total'))}；未调查 {_md(coverage.get('uninvestigated_total'))}。",
            "",
        ]
    )
    lines.extend(_finding_sections(analysis))
    return "\n".join(lines)


def render_specialty_reports(
    analysis: dict[str, Any], evidence_by_id: dict[str, dict[str, Any]]
) -> dict[str, str]:
    if not is_query_native(analysis):
        return {}
    quality = analysis.get("trace_quality", {})
    scan = analysis.get("query_scan", {})
    coverage = scan.get("coverage", {})
    reports: dict[str, str] = {}

    lines = [
        "# Trace Quality and Scan Coverage", "",
        f"- Trace quality：`{_md(quality.get('status'))}`",
        f"- Scan ID：`{_md(scan.get('scan_id'))}`",
        f"- Aggregate SHA：`{_md(scan.get('aggregate_content_sha256'))}`",
        f"- Candidate SHA：`{_md(scan.get('candidate_content_sha256'))}`",
        f"- Candidates：{_md(coverage.get('candidate_total'))}",
        f"- Required completed：{_md(coverage.get('required_completed'))}/{_md(coverage.get('required_total'))}",
        f"- Uninvestigated：{_md(coverage.get('uninvestigated_total'))}", "",
        "## Domain health", "", "| Domain | Status | Evidence |", "|---|---|---|",
    ]
    for item in sorted(analysis.get("domain_health", []), key=lambda value: str(value.get("domain"))):
        lines.append(f"| {_md(item.get('domain'))} | {_md(item.get('status'))} | {_md(', '.join(item.get('evidence_refs', [])))} |")
    lines.extend(["", "## 附录：录制质量与分析限制", "",
        "以下为数据可用性说明，不是性能候选，无 P0/P1/P2 排名，不占调查名额，也不默认执行根因深查。",
        "仅当具体性能候选依赖该缺失数据时，在该候选下关联 EvidenceGap 并按需补查。", "",
        "| 受影响域 | 状态 | 原始原因 | 分析限制 |", "|---|---|---|---|"])
    for item in analysis.get("capture_quality", []):
        lines.append(f"| {_md(item.get('domain'))} | {_md(item.get('status'))} | {_md(item.get('reason'))} | 不得依赖该失效/缺失数据确认性能根因；独立有效域仍可分析。 |")
    if not analysis.get("capture_quality"):
        lines.append("| — | — | Query 未返回质量附注 | 不代表所有未采集域均完整。 |")
    reports[SPECIALTY_REPORT_FILES[0]] = "\n".join(lines) + "\n"

    domain_specs = [
        (SPECIALTY_REPORT_FILES[1], "Frame and CPU Analysis", lambda f, s: str(s.get("stable_key", "")).startswith(("CPU", "Frame", "Main", "Managed", "Lua"))),
        (SPECIALTY_REPORT_FILES[2], "GPU and Render Analysis", lambda f, s: any(x in str(s.get("stable_key", "")) for x in ("GPU", "Render", "Present", "Submission"))),
        (SPECIALTY_REPORT_FILES[3], "Job and Scheduling Analysis", lambda f, s: any(x in str(s.get("stable_key", "")) for x in ("Job", "Scheduling", "ContextSwitch"))),
        (SPECIALTY_REPORT_FILES[4], "Memory and GPU Resource Analysis", lambda f, s: any(x in str(s.get("stable_key", "")) for x in ("Memory", "Resource", "Allocation", "Heap", "Catalog"))),
        (SPECIALTY_REPORT_FILES[5], "I/O, Sampling and System Analysis", lambda f, s: any(x in str(s.get("stable_key", "")) for x in ("IO", "I/O", "Sampling", "System", "Lock"))),
    ]
    for filename, title, predicate in domain_specs:
        reports[filename] = "\n".join([f"# {title}", "", *_finding_sections(analysis, predicate)]) + "\n"

    tracy = analysis.get("tracy_findings", {})
    lines = [
        "# Tracy Telemetry Overhead", "",
        f"- `TotalCaptureOverhead = {_md(tracy.get('total_capture_overhead'))}`", "",
        "该值表示单 Trace 没有未录制对照，不能证明总体录制开销为零或给出 A/B 百分比。", "",
        "## Runtime observations", "",
    ]
    lines.extend(f"- {_md(value)}" for value in tracy.get("runtime_findings", []))
    if not tracy.get("runtime_findings"):
        lines.append("- 无达到报告门槛的独立 Runtime 发现。")
    lines.extend(["", "## Offline analysis observations", ""])
    lines.extend(f"- {_md(value)}" for value in tracy.get("analysis_findings", []))
    if not tracy.get("analysis_findings"):
        lines.append("- 无达到报告门槛的独立离线分析发现。")
    reports[SPECIALTY_REPORT_FILES[6]] = "\n".join(lines) + "\n"

    signatures = {item.get("id"): item for item in analysis.get("bottleneck_signatures", []) if isinstance(item, dict)}
    lines = [
        "# Profiler Manual Verification Guide", "",
        "按下列 Candidate 的帧号和稳定实体人工回查。先定位 Frame，再展开对应线程或 GPU Queue；不要用 GUI 默认 inclusive 排名替代报告中的 Query scope。", "",
    ]
    for finding in analysis.get("candidate_findings", []):
        signature = signatures.get(finding.get("signature_id"), {})
        stable_key = str(signature.get("stable_key", finding.get("signature_id", "")))
        lines.extend([f"## {_md(finding.get('candidate_id'))}", "", f"- 稳定实体：`{_md(stable_key)}`"])
        if "GPU" in stable_key or "Render" in stable_key:
            lines.append("- 轨道：先展开 Render Thread，再展开 GPU Direct；存在 Compute/Copy 关系时分别核对真实 Queue。")
        else:
            lines.append("- 轨道：展开 Main Thread 和 Candidate relation 指向的 Worker/Render 线程。")
        for frame in finding.get("representative_frames", []):
            lines.append(
                f"- Frame {_md(frame.get('frame'))}：理由 `{_md(', '.join(frame.get('reasons', [])))}`；"
                f"事件 `{_md(', '.join(frame.get('event_refs', [])))}`。"
            )
        lines.extend([f"- Evidence：`{_md(', '.join(finding.get('evidence_refs', [])))}`", ""])
    reports[SPECIALTY_REPORT_FILES[7]] = "\n".join(lines) + "\n"

    lines = ["# Investigation Backlog and Evidence Gaps", "", "## Backlog", ""]
    backlog = analysis.get("investigation_backlog", [])
    if backlog:
        for item in backlog:
            lines.append(
                f"- `{_md(item.get('candidate_id'))}` / `{_md(item.get('priority'))}` / "
                f"`{_md(item.get('domain'))}`：{_md(item.get('reason', 'not investigated'))}"
            )
    else:
        lines.append("- 无。")
    lines.extend(["", "## Evidence Gaps", ""])
    lines.extend(render_evidence_gaps(analysis))
    reports[SPECIALTY_REPORT_FILES[8]] = "\n".join(lines) + "\n"
    return reports
