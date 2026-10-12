#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Build deterministic Markdown and static evidence from validated JN Tracy analysis JSON."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
import textwrap
from pathlib import Path
from typing import Any

from report_common import (
    ANALYSIS_WORKFLOW_VERSION,
    REPORT_BUILDER_VERSION,
    REPORT_MANIFEST_SCHEMA_VERSION,
    TEMPLATE_VERSION,
    ValidationFailure,
    format_ms,
    html_text,
    js_literal,
    load_json,
    md_text,
    render_minimum_evidence,
    sha256_file,
    stable_id,
    validate_analysis_and_evidence,
    validation_result,
    write_json,
)
from query_native_report import (
    SPECIALTY_REPORT_FILES,
    is_query_native,
    render_analysis_process,
    render_evidence_gaps,
    render_specialty_reports,
    validate_query_native_analysis,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--legacy-regression", action="store_true", help="Historical regression output, not current workflow completion")
    parser.add_argument("--analysis", required=True, type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--candidates", type=Path, help="Query Candidate Manifest; required by workflow 2.0")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--deterministic", action="store_true")
    parser.add_argument("--export-static", action="store_true")
    parser.add_argument("--html", action="store_true", help="also build the optional experimental HTML evidence viewer")
    parser.add_argument("--standalone", action="store_true")
    parser.add_argument("--validate-only", action="store_true")
    return parser.parse_args()


def bullet_lines(values: list[Any], empty: str = "无") -> list[str]:
    if not values:
        return [f"- {empty}"]
    return [f"- {md_text(value)}" for value in values]


def evidence_quality_label(evidence: dict[str, Any]) -> str:
    if evidence.get("truncated"):
        return "QueryTruncated"
    if evidence.get("partial"):
        return "QueryPartial"
    if evidence.get("quality") in {"unavailable", "degraded"}:
        return "EvidenceUnavailable"
    return "Complete"


def truncate_for_width(value: Any, max_width: float, font_size: float, max_chars: int | None = None) -> str:
    text = str(value or "")
    estimated_char_width = max(font_size * 0.58, 1.0)
    capacity = max(1, int(max_width / estimated_char_width))
    if max_chars is not None:
        capacity = min(capacity, max_chars)
    if len(text) <= capacity:
        return text
    if capacity <= 1:
        return "…"
    return text[: capacity - 1] + "…"


def wrap_svg_label(value: Any, width_chars: int = 42, max_lines: int = 3) -> list[str]:
    text = str(value or "Unknown")
    wrapped = textwrap.wrap(
        text,
        width=max(width_chars, 4),
        break_long_words=True,
        break_on_hyphens=False,
        replace_whitespace=True,
    ) or ["Unknown"]
    if len(wrapped) <= max_lines:
        return wrapped
    result = wrapped[:max_lines]
    result[-1] = truncate_for_width(result[-1] + "…", width_chars, 1.0, width_chars)
    return result


def format_bytes(value: int) -> str:
    amount = float(value)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if abs(amount) < 1024.0 or unit == "TiB":
            return f"{amount:.2f} {unit}"
        amount /= 1024.0
    return f"{amount:.2f} TiB"


def timeline_evidence(signature: dict[str, Any], evidence_by_id: dict[str, dict[str, Any]], data_by_id: dict[str, Any]) -> tuple[str | None, dict[str, Any] | None]:
    for evidence_id in signature.get("evidence_refs", []):
        evidence = evidence_by_id.get(evidence_id)
        if evidence and evidence.get("type") in {"correlated_timeline", "zone_timeline", "gpu_timeline"}:
            return evidence_id, data_by_id.get(evidence_id)
    return None, None


def render_timeline_svg(signature: dict[str, Any], evidence: dict[str, Any] | None, data: dict[str, Any] | None) -> str | None:
    width = 1280
    left = 230
    right = 20
    top = 48
    lane_header_height = 26
    row_height = 22
    row_pitch = 26
    lane_bottom_padding = 8
    if not evidence or not data:
        return None
    lanes = data.get("lanes", [])
    zones = data.get("zones", [])
    if not lanes or not zones:
        return None
    start_ns = int(data["start_ns"])
    end_ns = int(data["end_ns"])
    span = max(end_ns - start_ns, 1)
    lane_index = {str(lane.get("lane_id")): index for index, lane in enumerate(lanes)}
    zone_by_ref = {str(zone.get("zone_ref")): zone for zone in zones}

    def depth(zone: dict[str, Any]) -> int:
        seen: set[str] = set()
        current = zone
        result = 0
        while current.get("parent_ref") is not None:
            parent_ref = str(current.get("parent_ref"))
            if parent_ref in seen or parent_ref not in zone_by_ref:
                break
            seen.add(parent_ref)
            result += 1
            current = zone_by_ref[parent_ref]
        return result

    zones_by_lane: dict[str, list[dict[str, Any]]] = {str(lane.get("lane_id")): [] for lane in lanes}
    for zone in zones:
        lane_id = str(zone.get("lane_id"))
        if lane_id in zones_by_lane:
            zones_by_lane[lane_id].append(zone)

    zone_rows: dict[str, int] = {}
    lane_row_counts: dict[str, int] = {}
    for lane in lanes:
        lane_id = str(lane.get("lane_id"))
        row_ends: list[int] = []
        ordered = sorted(
            zones_by_lane.get(lane_id, []),
            key=lambda item: (depth(item), int(item.get("start_ns", 0)), -int(item.get("end_ns", 0)), str(item.get("zone_ref"))),
        )
        for zone in ordered:
            parent_row = zone_rows.get(str(zone.get("parent_ref")))
            first_row = parent_row + 1 if parent_row is not None else 0
            zone_start = int(zone.get("start_ns", 0))
            row = first_row
            while row < len(row_ends) and row_ends[row] > zone_start:
                row += 1
            while len(row_ends) <= row:
                row_ends.append(-(1 << 63))
            row_ends[row] = int(zone.get("end_ns", zone_start))
            zone_rows[str(zone.get("zone_ref"))] = row
        lane_row_counts[lane_id] = max(len(row_ends), 1)

    lane_offsets: dict[str, float] = {}
    cursor_y = float(top)
    for lane in lanes:
        lane_id = str(lane.get("lane_id"))
        lane_offsets[lane_id] = cursor_y
        cursor_y += lane_header_height + lane_row_counts[lane_id] * row_pitch + lane_bottom_padding
    height = int(cursor_y + 22)

    quality = evidence_quality_label(evidence)
    stable_key = str(signature.get("stable_key", ""))
    display_key = truncate_for_width(stable_key, 875, 16, 92)
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img" aria-label="{html_text(signature.get("stable_key"))}">',
        f'<rect width="{width}" height="{height}" fill="#111827"/>',
        f'<text x="18" y="27" fill="#e5e7eb" font-family="Segoe UI, sans-serif" font-size="16"><title>{html_text(stable_key)}</title>{html_text(display_key)}</text>',
        f'<text x="{width - 20}" y="27" text-anchor="end" fill="#93c5fd" font-family="Consolas, monospace" font-size="13">LOD: {quality}</text>',
    ]
    for index, lane in enumerate(lanes):
        lane_id = str(lane.get("lane_id"))
        y = lane_offsets[lane_id]
        lane_height = lane_header_height + lane_row_counts[lane_id] * row_pitch + lane_bottom_padding
        lane_label = str(lane.get("label", lane.get("lane_id")))
        lane_display = truncate_for_width(lane_label, left - 30, 13, 28)
        lines.append(f'<rect x="0" y="{y:.1f}" width="{width}" height="{lane_height}" fill="{("#172033" if index % 2 == 0 else "#131b2a")}"/>')
        lines.append(f'<text x="12" y="{y + 18:.1f}" fill="#cbd5e1" font-family="Segoe UI, sans-serif" font-size="13"><title>{html_text(lane_label)}</title>{html_text(lane_display)}</text>')
        for row in range(lane_row_counts[lane_id]):
            row_y = y + lane_header_height + row * row_pitch
            lines.append(f'<line x1="{left}" y1="{row_y + row_height:.1f}" x2="{width - right}" y2="{row_y + row_height:.1f}" stroke="#25324a" stroke-width="1"/>')
    colors = ["#60a5fa", "#34d399", "#f59e0b", "#f472b6", "#a78bfa"]
    for zone in sorted(zones, key=lambda item: (str(item.get("lane_id")), int(item.get("start_ns", 0)), -int(item.get("end_ns", 0)))):
        lane_id = str(zone.get("lane_id"))
        lane = lane_index.get(lane_id)
        if lane is None:
            continue
        x = left + ((int(zone["start_ns"]) - start_ns) / span) * (width - left - right)
        raw_width = ((int(zone["end_ns"]) - int(zone["start_ns"])) / span) * (width - left - right)
        zone_width = max(raw_width, 1.0)
        d = depth(zone)
        row = zone_rows.get(str(zone.get("zone_ref")), 0)
        y = lane_offsets[lane_id] + lane_header_height + row * row_pitch
        h = row_height
        raw_label = str(zone.get("label", zone.get("zone_ref")))
        display_label = truncate_for_width(raw_label, min(max(zone_width - 8, 1), 310), 11, 48)
        label = html_text(raw_label)
        zone_ref = html_text(zone.get("zone_ref"))
        lines.append(f'<g data-zone-ref="{zone_ref}"><rect x="{x:.3f}" y="{y:.1f}" width="{zone_width:.3f}" height="{h}" rx="2" fill="{colors[d % len(colors)]}" opacity="0.88"><title>{label}</title></rect>')
        if zone_width >= 28:
            lines.append(f'<text x="{x + 4:.3f}" y="{y + h - 6:.1f}" fill="#08111f" font-family="Segoe UI, sans-serif" font-size="11">{html_text(display_label)}</text>')
        lines.append('</g>')
    lines.append('</svg>')
    return "\n".join(lines) + "\n"


def render_causal_svg(signature: dict[str, Any]) -> str | None:
    chain = signature.get("causal_chain", [])
    if not chain:
        return None
    labels = [str(chain[0].get("from", chain[0].get("from_ref", "Unknown")))]
    labels.extend(str(edge.get("to", edge.get("to_ref", "Unknown"))) for edge in chain)
    width = 1280
    node_width = 660
    node_x = (width - node_width) / 2
    relation_gap = 58
    wrapped_labels = [wrap_svg_label(label, 42, 3) for label in labels]
    node_heights = [max(64, 26 + len(lines_for_label) * 19) for lines_for_label in wrapped_labels]
    height = 36 + sum(node_heights) + relation_gap * max(len(labels) - 1, 0) + 30
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img">',
        f'<rect width="{width}" height="{height}" fill="#111827"/>',
        '<defs><marker id="arrow" markerWidth="10" markerHeight="8" refX="9" refY="4" orient="auto"><path d="M0,0 L10,4 L0,8 z" fill="#93c5fd"/></marker></defs>',
    ]
    node_tops: list[float] = []
    cursor_y = 36.0
    for index, (label, label_lines, node_height) in enumerate(zip(labels, wrapped_labels, node_heights)):
        node_tops.append(cursor_y)
        lines.append(f'<g data-causal-node="{index}"><title>{html_text(label)}</title>')
        lines.append(f'<rect x="{node_x:.2f}" y="{cursor_y:.2f}" width="{node_width}" height="{node_height}" rx="8" fill="#1e3a5f" stroke="#60a5fa"/>')
        text_start = cursor_y + node_height / 2 - (len(label_lines) - 1) * 9.5
        lines.append(f'<text x="{width / 2:.2f}" y="{text_start:.2f}" text-anchor="middle" fill="#e5e7eb" font-family="Segoe UI, sans-serif" font-size="14">')
        for line_index, label_line in enumerate(label_lines):
            dy = 0 if line_index == 0 else 19
            lines.append(f'<tspan x="{width / 2:.2f}" dy="{dy}">{html_text(label_line)}</tspan>')
        lines.append('</text></g>')
        cursor_y += node_height + (relation_gap if index < len(labels) - 1 else 0)
    for index, edge in enumerate(chain):
        y1 = node_tops[index] + node_heights[index]
        y2 = node_tops[index + 1]
        center_x = width / 2
        lines.append(f'<line x1="{center_x:.2f}" y1="{y1:.2f}" x2="{center_x:.2f}" y2="{y2 - 8:.2f}" stroke="#93c5fd" stroke-width="2" marker-end="url(#arrow)"/>')
        lines.append(f'<text x="{center_x + 18:.2f}" y="{(y1 + y2) / 2 + 4:.2f}" fill="#fbbf24" font-family="Consolas, monospace" font-size="12">{html_text(edge.get("relation"))}</text>')
    lines.append('</svg>')
    return "\n".join(lines) + "\n"


def render_memory_svg(signature: dict[str, Any], data: dict[str, Any]) -> str | None:
    width, height, left, right, top, bottom = 1280, 360, 95, 25, 45, 52
    series = data.get("series", [])
    points = [point for item in series for point in item.get("points", [])]
    if not points:
        return None
    labels = [str(item.get("label") or item.get("name") or f"Series {index + 1}") for index, item in enumerate(series)]
    unique_times = {int(point["time_ns"]) for point in points}
    if len(unique_times) <= 1:
        values = [int(item.get("points", [{}])[-1].get("value_bytes", 0)) if item.get("points") else 0 for item in series]
        max_value = max(values + [int(item.get("budget_bytes", 0) or 0) for item in series] + [1])
        snapshot_left = 330
        snapshot_right = 90
        snapshot_top = 66
        bar_height = 30
        row_pitch = 58
        snapshot_height = snapshot_top + len(series) * row_pitch + 34
        colors = ["#60a5fa", "#34d399", "#f59e0b", "#f472b6"]
        title = truncate_for_width(str(signature.get("stable_key", "")) + " — GPU Memory Snapshot", 1160, 16, 104)
        lines = [
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {snapshot_height}" role="img" data-chart-kind="snapshot-bars">',
            f'<rect width="{width}" height="{snapshot_height}" fill="#111827"/>',
            f'<text x="18" y="27" fill="#e5e7eb" font-family="Segoe UI, sans-serif" font-size="16">{html_text(title)}</text>',
        ]
        plot_width = width - snapshot_left - snapshot_right
        for index, (label, value) in enumerate(zip(labels, values)):
            y = snapshot_top + index * row_pitch
            display_label = truncate_for_width(label, snapshot_left - 40, 13, 38)
            bar_width = value / max_value * plot_width
            lines.append(f'<text x="18" y="{y + 20}" fill="#cbd5e1" font-family="Segoe UI, sans-serif" font-size="13"><title>{html_text(label)}</title>{html_text(display_label)}</text>')
            lines.append(f'<rect x="{snapshot_left}" y="{y}" width="{plot_width}" height="{bar_height}" rx="4" fill="#1f2937"/>')
            lines.append(f'<rect x="{snapshot_left}" y="{y}" width="{bar_width:.2f}" height="{bar_height}" rx="4" fill="{colors[index % len(colors)]}"><title>{html_text(label)}: {format_bytes(value)}</title></rect>')
            lines.append(f'<text x="{min(snapshot_left + bar_width + 10, width - snapshot_right + 8):.2f}" y="{y + 20}" fill="#e5e7eb" font-family="Consolas, monospace" font-size="13">{html_text(format_bytes(value))}</text>')
        lines.append('</svg>')
        return "\n".join(lines) + "\n"
    min_time = min(int(point["time_ns"]) for point in points)
    max_time = max(int(point["time_ns"]) for point in points)
    max_value = max([int(point["value_bytes"]) for point in points] + [int(item.get("budget_bytes", 0) or 0) for item in series] + [1])
    time_span = max(max_time - min_time, 1)
    plot_width, plot_height = width - left - right, height - top - bottom
    colors = ["#60a5fa", "#34d399", "#f59e0b", "#f472b6"]
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img">',
        f'<rect width="{width}" height="{height}" fill="#111827"/>',
        f'<text x="18" y="27" fill="#e5e7eb" font-family="Segoe UI, sans-serif" font-size="16">{html_text(signature.get("stable_key"))} — Memory Trend</text>',
        f'<line x1="{left}" y1="{top + plot_height}" x2="{left + plot_width}" y2="{top + plot_height}" stroke="#64748b"/>',
        f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_height}" stroke="#64748b"/>',
    ]
    for index, item in enumerate(series):
        coords: list[str] = []
        for point in item.get("points", []):
            x = left + (int(point["time_ns"]) - min_time) / time_span * plot_width
            y = top + plot_height - int(point["value_bytes"]) / max_value * plot_height
            coords.append(f"{x:.2f},{y:.2f}")
        if coords:
            lines.append(f'<polyline points="{" ".join(coords)}" fill="none" stroke="{colors[index % len(colors)]}" stroke-width="2"><title>{html_text(item.get("label"))}</title></polyline>')
        budget = int(item.get("budget_bytes", 0) or 0)
        if budget > 0:
            y = top + plot_height - budget / max_value * plot_height
            lines.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_width}" y2="{y:.2f}" stroke="#ef4444" stroke-dasharray="8 5"><title>Budget {budget} bytes</title></line>')
        lines.append(f'<text x="{left + 18 + index * 220}" y="{height - 18}" fill="{colors[index % len(colors)]}" font-family="Segoe UI, sans-serif" font-size="13">{html_text(labels[index])}</text>')
    lines.append('</svg>')
    return "\n".join(lines) + "\n"


def render_resource_graph_svg(signature: dict[str, Any], data: dict[str, Any]) -> str | None:
    nodes = data.get("nodes", [])
    edges = data.get("edges", [])
    if not nodes:
        return None
    width, height = 1280, max(260, 120 + len(nodes) * 58)
    kind_order = {"pass": 0, "view": 1, "range": 2, "resource": 3, "allocation": 4, "heap": 5, "residency": 6}
    ordered = sorted(nodes, key=lambda node: (kind_order.get(str(node.get("kind", "")).lower(), 99), str(node.get("ref"))))
    positions: dict[str, tuple[float, float]] = {}
    lines = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img">', f'<rect width="{width}" height="{height}" fill="#111827"/>', '<defs><marker id="resource-arrow" markerWidth="10" markerHeight="8" refX="9" refY="4" orient="auto"><path d="M0,0 L10,4 L0,8 z" fill="#93c5fd"/></marker></defs>']
    for index, node in enumerate(ordered):
        column = kind_order.get(str(node.get("kind", "")).lower(), 7)
        x = 40 + column * 165
        y = 45 + index * 48
        positions[str(node.get("ref"))] = (x, y)
    for edge in edges:
        source = positions.get(str(edge.get("from_ref")))
        target = positions.get(str(edge.get("to_ref")))
        if not source or not target:
            continue
        lines.append(f'<line x1="{source[0] + 140:.2f}" y1="{source[1] + 18:.2f}" x2="{target[0] - 8:.2f}" y2="{target[1] + 18:.2f}" stroke="#93c5fd" stroke-width="1.5" marker-end="url(#resource-arrow)"><title>{html_text(edge.get("kind"))}</title></line>')
    for node in ordered:
        x, y = positions[str(node.get("ref"))]
        lines.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="140" height="36" rx="7" fill="#1e3a5f" stroke="#60a5fa"/>')
        lines.append(f'<text x="{x + 70:.2f}" y="{y + 23:.2f}" text-anchor="middle" fill="#e5e7eb" font-family="Segoe UI, sans-serif" font-size="11">{html_text(node.get("label", node.get("ref")))}</text>')
    lines.append('</svg>')
    return "\n".join(lines) + "\n"


def render_main_report(
    analysis: dict[str, Any],
    evidence_by_id: dict[str, dict[str, Any]],
    primary_visuals: dict[str, dict[str, str]],
    extra_visuals: dict[str, list[str]],
    html_enabled: bool,
) -> str:
    identity = analysis["report_identity"]
    scope = analysis["analysis_scope"]
    quality = analysis["trace_quality"]
    signatures = sorted(analysis["bottleneck_signatures"], key=lambda item: (-float(item.get("budget_debt", {}).get("total_ms", 0)), str(item.get("stable_key"))))
    lines = [
        f'# {md_text(identity["title"])}',
        "",
        "本报告由固定生成器从已验证的 Analysis Result 与 MCP Evidence 生成。Markdown 是权威结论；SVG/PNG 是静态证据附件。",
        "",
        "## 1. Trace 身份与证据质量",
        "",
        f'- Trace ID：`{md_text(identity["trace_id"])}`',
        f'- Trace SHA-256：`{md_text(identity["trace_sha256"])}`',
        f'- 分析范围：`{md_text(scope.get("mode"))}`，`{md_text(scope.get("start_ns"))}`–`{md_text(scope.get("end_ns"))}` ns',
        f'- 有效帧：{md_text(scope.get("valid_frame_count"))}；排除帧：{md_text(scope.get("excluded_frame_count"))}',
        f'- Trace 质量：`{md_text(quality.get("status"))}`',
        "- 配置身份：",
    ]
    for key, config in sorted(identity["configuration"].items()):
        lines.append(f'  - `{md_text(key)}`：`{md_text(config.get("path"))}` / `{md_text(config.get("sha256"))}`')
    if analysis.get("report_status") == "in_progress":
        lines[2:2] = ["**阶段报告：仍有入选候选未完成调查，不是完整性能验收结论。**", ""]
    for warning in quality.get("warnings", []):
        lines.append(f'- 质量警告：{md_text(warning)}')

    lines.extend(["", "## 2. 性能结论总览", ""])
    if signatures:
        for signature in signatures:
            lines.append(
                f'- **{md_text(signature["stable_key"])}** — {md_text(signature["primary_limiter"])}；'
                f'频次 {md_text(signature.get("frequency", {}).get("count"))}；累计 Budget Debt '
                f'{format_ms(signature.get("budget_debt", {}).get("total_ms"))}；结论 `{md_text(signature.get("conclusion_status"))}`。'
            )
    else:
        lines.append('- 尚未形成已调查的性能结论；未调查项目见调查清单。' if
                     analysis.get("report_status") == "in_progress" else
                     '- 未发现达到显著性门禁的 Bottleneck Signature。')

    process = render_analysis_process(analysis)
    if process:
        lines.extend(["", process])

    lines.extend(["", "## 3. Frame Class 分布", "", "| Frame Class | 帧数 | 比例 | Flags |", "|---|---:|---:|---|"])
    for item in sorted(analysis["frame_class_summary"], key=lambda value: str(value.get("class"))):
        lines.append(f'| {md_text(item.get("class"))} | {md_text(item.get("count"))} | {float(item.get("ratio", 0)):.3%} | {md_text(", ".join(item.get("flags", [])))} |')

    lines.extend(["", "## 4. Bottleneck Signature 地图", "", "| Signature | Primary Limiter | 严重性 | Debt/min | 深度 | 置信度 |", "|---|---|---|---:|---|---|"])
    for signature in signatures:
        lines.append(
            f'| [{md_text(signature["stable_key"])}](#card-{stable_id(signature["id"]).lower()}) | '
            f'{md_text(signature["primary_limiter"])} | {md_text(signature["severity"])} | '
            f'{format_ms(signature.get("budget_debt", {}).get("per_minute_ms"))} | '
            f'{md_text(signature["deepest_level"])} | {md_text(signature["confidence"])} |'
        )

    lines.extend(["", "## 5. 关键路径与等待传播", ""])
    for signature in signatures:
        if not signature.get("causal_chain"):
            continue
        chain = " → ".join(f'{edge.get("from")} —{edge.get("relation")}→ {edge.get("to")}' for edge in signature["causal_chain"])
        lines.append(f'- `{md_text(signature["stable_key"])}`：{md_text(chain)}')
    if not any(signature.get("causal_chain") for signature in signatures):
        lines.append('- 没有已证明的跨线程或跨 Queue 因果链；相关结论应见 Evidence Gaps。')

    lines.extend(["", "## 6. 独立 Evidence Cards", ""])
    for signature in signatures:
        anchor = stable_id(signature["id"]).lower()
        signature_visuals = primary_visuals.get(signature["id"], {})
        lines.extend(
            [
                f'<a id="card-{anchor}"></a>',
                f'### {md_text(signature["stable_key"])}',
                "",
                f'- 影响：严重性 `{md_text(signature["severity"])}`；频次 {md_text(signature.get("frequency", {}).get("count"))}；累计 Debt {format_ms(signature.get("budget_debt", {}).get("total_ms"))}。',
                f'- Primary Limiter：`{md_text(signature["primary_limiter"])}`；Secondary：{md_text(", ".join(signature.get("secondary_contributors", [])) or "无")}。',
                f'- 深度：`{md_text(signature["deepest_level"])}`；置信度：`{md_text(signature["confidence"])}`；状态：`{md_text(signature["conclusion_status"])}`。',
            ]
        )
        if signature_visuals.get("timeline"):
            lines.append(f'![{md_text(signature["stable_key"])} Timeline]({signature_visuals["timeline"]})')
        else:
            lines.append('- 静态 Timeline：未生成（没有包含可验证 Zone 坐标的 Timeline Evidence）。')
        if signature_visuals.get("causal"):
            lines.append(f'![{md_text(signature["stable_key"])} Causal Chain]({signature_visuals["causal"]})')
        else:
            lines.append('- 因果链图：未生成（没有已证明的 typed relation）。')
        lines.extend(["", "代表实例：", ""])
        if html_enabled:
            lines.insert(len(lines) - 2, f'- 可选交互证据：[打开 HTML Evidence](visuals/index.html#signature-{stable_id(signature["id"])})')
        for visual_path in extra_visuals.get(signature["id"], []):
            lines.append(f'![{md_text(signature["stable_key"])} Evidence]({visual_path})')
        if extra_visuals.get(signature["id"]):
            lines.append("")
        for instance in signature.get("representative_instances", []):
            lines.append(f'- `{md_text(instance.get("role"))}`：Frame {md_text(instance.get("frame_id"))}；Evidence {md_text(", ".join(instance.get("evidence_refs", [])))}')
        lines.extend(["", "支持证据：", ""] + bullet_lines(signature.get("supported_claims", [])))
        lines.extend(["", "反证与未支持结论：", ""] + bullet_lines(signature.get("unsupported_claims", [])))
        lines.extend(["", "假设账本：", ""])
        for hypothesis in signature.get("hypothesis_ledger", []):
            lines.append(
                f'- `{md_text(hypothesis.get("status"))}` {md_text(hypothesis.get("hypothesis"))}；支持：'
                f'{md_text(", ".join(hypothesis.get("supporting_evidence", [])) or "无")}；反证：'
                f'{md_text(", ".join(hypothesis.get("contradicting_evidence", [])) or "无")}。'
            )
        lines.extend(["", "调用栈 / Source provenance：", ""] + bullet_lines(signature.get("stack_source_provenance", [])))
        lines.extend(["", "优化方向：", ""] + bullet_lines(signature.get("optimization_direction", [])))
        lines.extend(["", "功能风险：", ""] + bullet_lines(signature.get("functional_risk", [])))
        upper = signature.get("theoretical_upper_bound", {})
        lines.extend(
            [
                "",
                f'- 理论收益上限：{format_ms(upper.get("value_ms"))}；依据 `{md_text(upper.get("basis"))}`。',
                "- 复测指标：" + md_text("；".join(signature.get("retest_metrics", []))),
            ]
        )
        if signature.get("evidence_gap"):
            gap = signature["evidence_gap"]
            lines.append(f'- EvidenceGap：`{md_text(gap.get("reason"))}` / `{md_text(gap.get("blocked_by"))}`；最小补充证据：{render_minimum_evidence(gap.get("minimum_additional_evidence", []), separator=", ")}。')
        lines.append("")

    lines.extend(["## 7. CPU 内存与 GPU 显存专项", "", "### 7.1 CPU 内存", ""])
    if analysis["cpu_memory_findings"]:
        for finding in analysis["cpu_memory_findings"]:
            lines.append(f'- `{md_text(finding.get("classification"))}` {md_text(finding.get("summary"))}（Evidence: {md_text(", ".join(finding.get("evidence_refs", [])))})')
    else:
        lines.append('- 未形成达到显著性门禁的 CPU 内存结论。')

    lines.extend(["", "### 7.2 GPU 显存", ""])
    if analysis["gpu_memory_findings"]:
        for finding in analysis["gpu_memory_findings"]:
            lines.append(f'- `{md_text(finding.get("classification"))}` {md_text(finding.get("summary"))}（Evidence: {md_text(", ".join(finding.get("evidence_refs", [])))})')
    else:
        lines.append('- 未形成达到显著性门禁的 GPU 显存结论。')

    tracy = analysis["tracy_findings"]
    lines.extend(["", "## 8. Tracy 运行时采集与离线分析压力", "", f'- `TotalCaptureOverhead = {md_text(tracy.get("total_capture_overhead"))}`', "", "### 8.1 Tracy Runtime", ""])
    lines.extend(bullet_lines(tracy.get("runtime_findings", []), "单 Trace 中未发现独立的 Tracy Runtime 热点；这不等于总体录制开销为零。"))
    lines.extend(["", "### 8.2 Tracy Analysis", ""])
    lines.extend(bullet_lines(tracy.get("analysis_findings", []), "未发现 Query、转换或 Profiler 的独立离线分析压力。"))

    lines.extend(["", "## 9. Evidence Gaps", ""])
    lines.extend(render_evidence_gaps(analysis))

    lines.extend(["", "## 10. 优化优先级和复测方案", ""])
    for index, signature in enumerate(signatures, start=1):
        lines.append(f'{index}. `{md_text(signature["stable_key"])}`：{md_text("；".join(signature.get("optimization_direction", [])))}。复测：{md_text("；".join(signature.get("retest_metrics", [])))}。')
    if not signatures:
        lines.append('1. 当前没有可执行优化项；先补齐 EvidenceGap 或扩大用户指定范围。')

    lines.extend(["", "## 11. 全域健康统计附录", "", "| 域 | 状态 | Evidence |", "|---|---|---|"])
    for domain in sorted(analysis["domain_health"], key=lambda item: str(item.get("domain"))):
        lines.append(f'| {md_text(domain.get("domain"))} | {md_text(domain.get("status"))} | {md_text(", ".join(domain.get("evidence_refs", [])))} |')

    if is_query_native(analysis):
        lines.extend(["", "### 录制质量与分析限制", "",
            "质量附注不参与性能优先级排序，不占候选/调查名额，不默认深查。仅在具体性能问题依赖缺失数据时关联补查。", "",
            "| 受影响域 | 状态 | 原始原因 | 分析限制 |", "|---|---|---|---|"])
        for item in analysis.get("capture_quality", []):
            lines.append(f'| {md_text(item.get("domain"))} | {md_text(item.get("status"))} | {md_text(item.get("reason"))} | 不依赖该失效/缺失数据确认根因；独立有效域继续分析。 |')
        if not analysis.get("capture_quality"):
            lines.append("| — | — | Query 未返回质量附注 | 不代表所有未采集域均完整。 |")

    lines.extend(["", "## 12. 查询审计索引", "", "详见 [Analysis-Process-and-Query-Audit.md](Analysis-Process-and-Query-Audit.md) 与 [Evidence-Index.md](Evidence-Index.md)。", ""])
    if is_query_native(analysis):
        lines.extend(["## 13. 专项报告", ""])
        for name in SPECIALTY_REPORT_FILES:
            lines.append(f"- [{md_text(name.removesuffix('.md'))}]({name})")
        lines.append("")
    return "\n".join(lines)


def render_query_audit(analysis: dict[str, Any], evidence_by_id: dict[str, dict[str, Any]]) -> str:
    lines = [
        "# Analysis Process and Query Audit",
        "",
        "| Evidence ID | MCP 方法 | Attempt | Quality | Partial | Truncated | Data |",
        "|---|---|---:|---|---|---|---|",
    ]
    for evidence_id in sorted(evidence_by_id):
        item = evidence_by_id[evidence_id]
        lines.append(
            f'| `{md_text(evidence_id)}` | `{md_text(item.get("mcp", {}).get("method"))}` | {md_text(item.get("attempt"))} | '
            f'{md_text(item.get("quality"))} | {md_text(item.get("partial"))} | {md_text(item.get("truncated"))} | '
            f'`{md_text(item.get("data_path"))}` |'
        )
    lines.extend(["", "每个 MCP 请求最多尝试 3 次；第三次失败必须以 QueryUnavailable EvidenceGap 出现在主报告。", ""])
    return "\n".join(lines)


def render_evidence_index(analysis: dict[str, Any], evidence_by_id: dict[str, dict[str, Any]]) -> str:
    signature_refs: dict[str, list[str]] = {}
    for signature in analysis["bottleneck_signatures"]:
        for evidence_id in signature.get("evidence_refs", []):
            signature_refs.setdefault(evidence_id, []).append(signature["id"])
    lines = ["# Evidence Index", "", "| Evidence ID | 类型 | Trace | Frame/Time refs | Signatures |", "|---|---|---|---|---|"]
    for evidence_id in sorted(evidence_by_id):
        item = evidence_by_id[evidence_id]
        refs = item.get("refs", {})
        scope = f'frames={refs.get("frames", [])}; time={refs.get("time_ranges_ns", [])}'
        lines.append(
            f'| `{md_text(evidence_id)}` | {md_text(item.get("type"))} | `{md_text(item.get("trace_id"))}` | '
            f'{md_text(scope)} | {md_text(", ".join(signature_refs.get(evidence_id, [])))} |'
        )
    lines.append("")
    return "\n".join(lines)


HTML_STYLE = """
:root{color-scheme:dark;--bg:#0b1020;--panel:#111827;--muted:#94a3b8;--text:#e5e7eb;--accent:#60a5fa;--warn:#f59e0b}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.55 "Segoe UI",sans-serif}
header{position:sticky;top:0;z-index:2;padding:16px 24px;background:#0b1020e8;border-bottom:1px solid #253047}main{max-width:1500px;margin:auto;padding:24px}
.card{background:var(--panel);border:1px solid #26334d;border-radius:10px;padding:18px;margin:0 0 24px}.meta{color:var(--muted)}
.timeline{overflow:auto;border:1px solid #27354e;background:#0b1020}.timeline svg{display:block;min-width:100%}.badge{display:inline-block;padding:2px 8px;border-radius:999px;background:#1f2b44;color:#bfdbfe;margin-right:6px}
button,input{accent-color:var(--accent)}a{color:#93c5fd}.warning{color:#fbbf24}.chain{font-family:Consolas,monospace;white-space:pre-wrap}
""".strip()


HTML_APP = r"""
(function(){
  const store=window.JNTRACY_EVIDENCE||{};
  function esc(s){return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
  function render(host,data,zoom){
    if(!data){host.innerHTML='<p class="warning">EvidenceUnavailable</p>';return;}
    const lanes=data.lanes||[], zones=data.zones||[], start=Number(data.start_ns), end=Number(data.end_ns), span=Math.max(end-start,1);
    const laneMap=new Map(lanes.map((l,i)=>[String(l.lane_id),i])); const width=1400,left=190,row=44,height=50+Math.max(lanes.length,1)*row;
    let svg=`<svg viewBox="0 0 ${width} ${height}" role="img">`;
    lanes.forEach((lane,i)=>{const y=35+i*row;svg+=`<rect x="0" y="${y}" width="${width}" height="${row}" fill="${i%2?'#131b2a':'#172033'}"/><text x="12" y="${y+27}" fill="#cbd5e1">${esc(lane.label||lane.lane_id)}</text>`;});
    const density=new Map();
    zones.forEach((z,i)=>{const lane=laneMap.get(String(z.lane_id));if(lane===undefined)return;const x=left+(Number(z.start_ns)-start)/span*(width-left-20);const raw=(Number(z.end_ns)-Number(z.start_ns))/span*(width-left-20);if(raw*zoom/100<1){const bin=Math.floor(x/2);const key=`${lane}:${bin}`;density.set(key,{lane,bin,count:(density.get(key)?.count||0)+1});return;}const w=Math.max(raw,0.25);const y=40+lane*row;svg+=`<rect data-zone-ref="${esc(z.zone_ref)}" x="${x}" y="${y}" width="${w}" height="30" rx="2" fill="${['#60a5fa','#34d399','#f59e0b','#f472b6'][i%4]}"><title>${esc(z.label||z.zone_ref)} | ${z.start_ns}–${z.end_ns} ns</title></rect>`;});
    density.forEach(d=>{const x=d.bin*2,y=40+d.lane*row,opacity=Math.min(.25+Math.log2(d.count+1)/5,.95);svg+=`<rect x="${x}" y="${y}" width="2" height="30" fill="#a78bfa" opacity="${opacity}"><title>VisualAggregationOnly: ${d.count} zones; zoom to reveal real zones</title></rect>`;});
    host.innerHTML=svg+'</svg>';host.querySelector('svg').style.width=`${zoom}%`;
  }
  function activate(host){if(host.dataset.loaded)return;host.dataset.loaded='1';const done=()=>{const range=host.previousElementSibling?.querySelector('input[type=range]');const redraw=()=>render(host,store[host.dataset.evidenceKey],Number(range?.value||100));if(range)range.addEventListener('input',redraw);redraw();};if(store[host.dataset.evidenceKey]){done();return;}const script=document.createElement('script');script.src=host.dataset.script;script.onload=done;script.onerror=()=>{host.innerHTML='<p class="warning">EvidenceUnavailable: data shard failed to load</p>';};document.body.appendChild(script);}
  const hosts=[...document.querySelectorAll('[data-evidence-key]')];
  if('IntersectionObserver' in window){const observer=new IntersectionObserver(entries=>entries.forEach(entry=>{if(entry.isIntersecting){activate(entry.target);observer.unobserve(entry.target);}}),{rootMargin:'400px'});hosts.forEach(host=>observer.observe(host));}else{hosts.forEach(activate);}
})();
""".strip()


def render_html(analysis: dict[str, Any], evidence_by_id: dict[str, dict[str, Any]], signature_keys: dict[str, str], primary_visuals: dict[str, dict[str, str]], extra_visuals: dict[str, list[str]], standalone: bool = False) -> str:
    identity = analysis["report_identity"]
    signatures = sorted(analysis["bottleneck_signatures"], key=lambda item: str(item["id"]))
    script_tags = ""
    cards: list[str] = []
    for signature in signatures:
        sid = stable_id(signature["id"])
        key = signature_keys.get(signature["id"], "")
        signature_visuals = primary_visuals.get(signature["id"], {})
        chain = " → ".join(f'{edge.get("from")} —{edge.get("relation")}→ {edge.get("to")}' for edge in signature.get("causal_chain", []))
        if standalone and signature_visuals.get("timeline"):
            visual = f'<img src="assets/static-previews/signature-{sid}-timeline.svg" alt="Timeline" style="max-width:100%">'
        elif not standalone and signature_visuals.get("timeline"):
            visual = f'<div><label>缩放 <input type="range" min="100" max="800" value="100"></label></div><div class="timeline" data-evidence-key="{html_text(key)}" data-script="assets/data/signature-{sid}.js"><p class="meta">滚动到此处后按需加载证据…</p></div>'
        else:
            visual = '<p class="warning">静态 Timeline 未生成：没有可验证 Zone 坐标。</p>'
        if signature_visuals.get("causal"):
            causal_visual = f'<img src="assets/static-previews/signature-{sid}-causal.svg" alt="Causal chain" style="max-width:100%">'
        else:
            causal_visual = '<p class="warning">因果链图未生成：没有已证明的 typed relation。</p>'
        extras = "".join(f'<img src="../{html_text(path)}" alt="Evidence" style="max-width:100%;display:block;margin-top:12px">' for path in extra_visuals.get(signature["id"], []))
        cards.append(
            f'<section class="card" id="signature-{sid}"><h2>{html_text(signature["stable_key"])}</h2>'
            f'<p><span class="badge">{html_text(signature["primary_limiter"])}</span><span class="badge">{html_text(signature["deepest_level"])}</span><span class="badge">{html_text(signature["confidence"])}</span></p>'
            f'<p class="meta">频次 {html_text(signature.get("frequency", {}).get("count"))} · Debt {html_text(format_ms(signature.get("budget_debt", {}).get("total_ms")))}</p>'
            f'{visual}{extras}<h3>因果链</h3>{causal_visual}<p class="chain">{html_text(chain or "EvidenceGap / no proven relation")}</p>'
            f'<h3>优化方向</h3><ul>{"".join(f"<li>{html_text(value)}</li>" for value in signature.get("optimization_direction", []))}</ul></section>'
        )
    summary = "".join(cards) or '<section class="card"><p>没有显著 Bottleneck Signature。</p></section>'
    return f"""<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{html_text(identity["title"])} — Evidence</title><style>{HTML_STYLE}</style></head>
<body><header><strong>{html_text(identity["title"])}</strong><div class="meta">Trace {html_text(identity["trace_id"])} · Markdown 为权威结论</div></header>
<main>{summary}</main>{script_tags}<script>{HTML_APP if not standalone else ''}</script></body></html>
"""


def copy_evidence_data(output: Path, evidence_path: Path, evidence_by_id: dict[str, dict[str, Any]]) -> None:
    for item in evidence_by_id.values():
        source = (evidence_path.parent / item["data_path"]).resolve()
        destination = output / item["data_path"]
        destination.parent.mkdir(parents=True, exist_ok=True)
        if source.resolve() != destination.resolve():
            shutil.copyfile(source, destination)
        receipt = item.get("mcp_receipt", item if item.get("type") == "mcp_query" else {})
        for kind in ("request", "response"):
            if kind not in receipt:
                continue
            record = receipt[kind]
            relative = Path(record["path"])
            source = (evidence_path.parent / relative).resolve()
            destination = (output / relative).resolve()
            if relative.is_absolute() or not source.is_relative_to(evidence_path.parent.resolve()) or not destination.is_relative_to(output.resolve()):
                raise ValidationFailure(["raw MCP receipt path escapes report/evidence root"])
            if sha256_file(source) != record["sha256"]:
                raise ValidationFailure(["raw MCP receipt checksum mismatch before bundling"])
            destination.parent.mkdir(parents=True, exist_ok=True)
            if source != destination:
                shutil.copyfile(source, destination)


def validate_rendered_report(output: Path, analysis: dict[str, Any], html_enabled: bool) -> dict[str, Any]:
    errors: list[str] = []
    report = (output / "Performance-Analysis-Report.md").read_text(encoding="utf-8")
    html_path = output / "visuals" / "index.html"
    html_index = html_path.read_text(encoding="utf-8") if html_enabled else ""
    for signature in analysis["bottleneck_signatures"]:
        sid = stable_id(signature["id"])
        if md_text(signature["stable_key"]) not in report:
            errors.append(f"Markdown missing signature {signature['id']}")
        if html_enabled:
            if f"visuals/index.html#signature-{sid}" not in report:
                errors.append(f"Markdown missing HTML anchor for {signature['id']}")
            if f'id="signature-{sid}"' not in html_index:
                errors.append(f"HTML missing signature anchor for {signature['id']}")
    if "TotalCaptureOverhead = NotMeasuredSingleTrace" not in report:
        errors.append("Markdown missing single-trace overhead boundary")
    if is_query_native(analysis):
        if "Query 原生候选与分析过程" not in report:
            errors.append("Markdown missing Query-native analysis process")
        for name in SPECIALTY_REPORT_FILES:
            if not (output / name).is_file():
                errors.append(f"specialty report is missing: {name}")
    if html_enabled and ("https://" in html_index or "http://" in html_index):
        errors.append("HTML references a network resource")
    for match in re.finditer(r"\[[^\]]*\]\(([^)]+)\)", report):
        target = match.group(1)
        if target.startswith(("http://", "https://", "#")):
            continue
        relative, _, anchor = target.partition("#")
        linked = (output / relative).resolve()
        if output.resolve() not in linked.parents and linked != output.resolve():
            errors.append(f"Markdown link escapes report output: {target}")
            continue
        if not linked.is_file():
            errors.append(f"Markdown link target is missing: {target}")
            continue
        if anchor:
            linked_text = linked.read_text(encoding="utf-8", errors="replace")
            if f'id="{anchor}"' not in linked_text:
                errors.append(f"Markdown link anchor is missing: {target}")
    for attribute, target in re.findall(r"\b(href|src)=\"([^\"]+)\"", html_index) if html_enabled else []:
        if target.startswith(("http://", "https://", "#", "data:")):
            continue
        relative, _, _ = target.partition("#")
        linked = (output / "visuals" / relative).resolve()
        if (output / "visuals").resolve() not in linked.parents:
            errors.append(f"HTML {attribute} escapes visuals output: {target}")
        elif not linked.is_file():
            errors.append(f"HTML {attribute} target is missing: {target}")
    return validation_result(analysis, errors)


def build(args: argparse.Namespace) -> int:
    analysis = load_json(args.analysis)
    evidence = load_json(args.evidence)
    errors, evidence_by_id, data_by_id = validate_analysis_and_evidence(analysis, evidence, args.evidence.parent)
    candidate_manifest: dict[str, Any] | None = None
    if is_query_native(analysis) or args.candidates:
        if not args.candidates:
            errors.append("--candidates is required for Query-native workflow 2.0")
        else:
            from candidate_manifest import load_candidate_manifest
            candidate_manifest = load_candidate_manifest(args.candidates)
            if not isinstance(candidate_manifest, dict):
                errors.append("Candidate Manifest must be an object")
            else:
                errors.extend(validate_query_native_analysis(analysis, candidate_manifest, evidence, args.evidence.parent))
                expected_sha = analysis.get("query_scan", {}).get("candidate_manifest_sha256")
                actual_sha = sha256_file(args.candidates)
                if expected_sha != actual_sha:
                    errors.append("analysis.query_scan.candidate_manifest_sha256 does not match --candidates")
                records = candidate_manifest.get("candidate_records", {})
                record_path = records.get("path")
                if record_path:
                    for item in evidence_by_id.values():
                        evidence_paths = [item.get("data_path")]
                        evidence_paths += [r.get("path") for r in item.get("mcp_receipt", {}).values() if isinstance(r, dict)]
                        if record_path in evidence_paths:
                            errors.append("candidate_records path collides with evidence artifact")
    model = {}
    if candidate_manifest is not None and not args.legacy_regression:
        from performance_review import validate_review
        new_errors, model = validate_review(analysis, candidate_manifest, evidence, args.evidence.parent)
        errors.extend(new_errors)
    if errors:
        raise ValidationFailure(errors)
    if candidate_manifest is not None and not args.legacy_regression:
        from performance_report import status, build_review_report
        if args.validate_only:
            print(json.dumps(status(analysis,model),ensure_ascii=False,sort_keys=True))
            return 0
        if not args.output:
            raise ValidationFailure(["--output is required unless --validate-only is used"])
        return build_review_report(args,analysis,candidate_manifest,evidence,model)
    if args.legacy_regression:
        analysis = dict(analysis, legacy_regression=True)
    if args.validate_only:
        print(json.dumps(validation_result(analysis, []), ensure_ascii=False, sort_keys=True))
        return 0
    if not args.output:
        raise ValidationFailure(["--output is required unless --validate-only is used"])

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if args.standalone and not args.html:
        raise ValidationFailure(["--standalone requires --html"])
    visuals = output / "visuals"
    data_dir = visuals / "assets" / "data"
    preview_dir = visuals / "assets" / "static-previews"
    frame_image_dir = visuals / "assets" / "frame-images"
    export_dir = visuals / "assets" / "exports"
    for directory in (data_dir, preview_dir, frame_image_dir, export_dir, output / "evidence-data"):
        directory.mkdir(parents=True, exist_ok=True)
    for stale_preview in preview_dir.glob("signature-*.svg"):
        stale_preview.unlink()
    for stale_frame_image in frame_image_dir.glob("signature-*-frame-*.png"):
        stale_frame_image.unlink()
    if not args.html:
        for stale_shard in data_dir.glob("signature-*.js"):
            stale_shard.unlink()

    write_json(output / "analysis-result.json", analysis)
    write_json(output / "evidence-manifest.json", evidence)
    candidate_artifacts = []
    if candidate_manifest is not None:
        from candidate_manifest import copy_candidate_manifest
        candidate_artifacts = copy_candidate_manifest(args.candidates, output)
    copy_evidence_data(output, args.evidence, evidence_by_id)

    source_artifacts = set()
    for finding in analysis.get("candidate_findings", []):
        for source in finding.get("source_evidence", []):
            snapshot = source.get("snapshot")
            if not snapshot:
                continue
            relative = Path(snapshot["path"])
            original = (args.evidence.parent / relative).resolve()
            destination = (output / relative).resolve()
            if relative.is_absolute() or not original.is_relative_to(args.evidence.parent.resolve()) or not destination.is_relative_to(output.resolve()):
                raise ValidationFailure(["source snapshot path escapes evidence/report root"])
            if sha256_file(original) != snapshot["sha256"]:
                raise ValidationFailure(["source snapshot checksum mismatch before bundling"])
            destination.parent.mkdir(parents=True, exist_ok=True)
            if original != destination:
                shutil.copyfile(original, destination)
            source_artifacts.add(destination)

    signature_keys: dict[str, str] = {}
    primary_visuals: dict[str, dict[str, str]] = {}
    extra_visuals: dict[str, list[str]] = {}
    generated_paths: set[Path] = {output / "analysis-result.json", output / "evidence-manifest.json"}
    generated_paths.update(candidate_artifacts)
    generated_paths.update(source_artifacts)
    for item in evidence_by_id.values():
        generated_paths.add(output / item["data_path"])
        for record in item.get("mcp_receipt", {k: item[k] for k in ("request", "response") if k in item} if item.get("type") == "mcp_query" else {}).values():
            if isinstance(record, dict) and "path" in record:
                generated_paths.add(output / record["path"])
    for signature in analysis["bottleneck_signatures"]:
        evidence_id, timeline = timeline_evidence(signature, evidence_by_id, data_by_id)
        sid = stable_id(signature["id"])
        key = "signature-" + sid
        signature_keys[signature["id"]] = key
        if args.html:
            shard = data_dir / f"signature-{sid}.js"
            shard.write_text(f'window.JNTRACY_EVIDENCE=window.JNTRACY_EVIDENCE||{{}};window.JNTRACY_EVIDENCE[{js_literal(key)}]={js_literal(timeline)};\n', encoding="utf-8")
            generated_paths.add(shard)
        timeline_svg = render_timeline_svg(signature, evidence_by_id.get(evidence_id) if evidence_id else None, timeline)
        if timeline_svg is not None:
            preview = preview_dir / f"signature-{sid}-timeline.svg"
            preview.write_text(timeline_svg, encoding="utf-8")
            generated_paths.add(preview)
            primary_visuals.setdefault(signature["id"], {})["timeline"] = f"visuals/assets/static-previews/{preview.name}"
        causal_svg = render_causal_svg(signature)
        if causal_svg is not None:
            causal_preview = preview_dir / f"signature-{sid}-causal.svg"
            causal_preview.write_text(causal_svg, encoding="utf-8")
            generated_paths.add(causal_preview)
            primary_visuals.setdefault(signature["id"], {})["causal"] = f"visuals/assets/static-previews/{causal_preview.name}"

        for extra_evidence_id in signature.get("evidence_refs", []):
            extra_evidence = evidence_by_id.get(extra_evidence_id)
            extra_data = data_by_id.get(extra_evidence_id)
            if not extra_evidence or not isinstance(extra_data, dict):
                continue
            extra_type = extra_evidence.get("type")
            if extra_type == "memory_timeline":
                memory_svg = render_memory_svg(signature, extra_data)
                if memory_svg is not None:
                    memory_preview = preview_dir / f"signature-{sid}-memory.svg"
                    memory_preview.write_text(memory_svg, encoding="utf-8")
                    generated_paths.add(memory_preview)
                    extra_visuals.setdefault(signature["id"], []).append(f"visuals/assets/static-previews/{memory_preview.name}")
            elif extra_type == "resource_relation_graph":
                resource_svg = render_resource_graph_svg(signature, extra_data)
                if resource_svg is not None:
                    resource_preview = preview_dir / f"signature-{sid}-resources.svg"
                    resource_preview.write_text(resource_svg, encoding="utf-8")
                    generated_paths.add(resource_preview)
                    extra_visuals.setdefault(signature["id"], []).append(f"visuals/assets/static-previews/{resource_preview.name}")
            elif extra_type == "frame_image":
                source_image = (args.evidence.parent / str(extra_data.get("image_path"))).resolve()
                frame_id = stable_id(str(extra_data.get("frame_id", "unknown")))
                destination_image = frame_image_dir / f"signature-{sid}-frame-{frame_id}.png"
                shutil.copyfile(source_image, destination_image)
                generated_paths.add(destination_image)
                extra_visuals.setdefault(signature["id"], []).append(f"visuals/assets/frame-images/{destination_image.name}")

    report_path = output / "Performance-Analysis-Report.md"
    audit_path = output / "Analysis-Process-and-Query-Audit.md"
    index_path = output / "Evidence-Index.md"
    report_path.write_text(render_main_report(analysis, evidence_by_id, primary_visuals, extra_visuals, args.html), encoding="utf-8")
    audit_path.write_text(render_query_audit(analysis, evidence_by_id), encoding="utf-8")
    index_path.write_text(render_evidence_index(analysis, evidence_by_id), encoding="utf-8")
    generated_paths.update({report_path, audit_path, index_path})
    for name, content in render_specialty_reports(analysis, evidence_by_id).items():
        specialty_path = output / name
        specialty_path.write_text(content, encoding="utf-8")
        generated_paths.add(specialty_path)

    html_path = visuals / "index.html"
    standalone_path = visuals / "standalone.html"
    if args.html:
        html_path.write_text(render_html(analysis, evidence_by_id, signature_keys, primary_visuals, extra_visuals, standalone=False), encoding="utf-8")
        generated_paths.add(html_path)
        if args.standalone:
            standalone_path.write_text(render_html(analysis, evidence_by_id, signature_keys, primary_visuals, extra_visuals, standalone=True), encoding="utf-8")
            generated_paths.add(standalone_path)
        elif standalone_path.is_file():
            standalone_path.unlink()
    else:
        for stale_html in (html_path, standalone_path):
            if stale_html.is_file():
                stale_html.unlink()

    validation = validate_rendered_report(output, analysis, args.html)
    validation_path = output / "report-validation.json"
    write_json(validation_path, validation)
    generated_paths.add(validation_path)
    if not validation["passed"]:
        raise ValidationFailure(validation["errors"])

    manifest = {
        "schema_version": REPORT_MANIFEST_SCHEMA_VERSION,
        "report_id": analysis["report_identity"]["report_id"],
        "trace_id": analysis["report_identity"]["trace_id"],
        "trace_sha256": analysis["report_identity"]["trace_sha256"],
        "generated_at": analysis["report_identity"]["generated_at"],
        "deterministic": bool(args.deterministic),
        "versions": {
            "analysis_result_schema": analysis["schema_version"],
            "evidence_manifest_schema": evidence["schema_version"],
            "report_builder": REPORT_BUILDER_VERSION,
            "template": TEMPLATE_VERSION,
            "analysis_workflow": ANALYSIS_WORKFLOW_VERSION,
        },
        "configuration": analysis["report_identity"]["configuration"],
        "artifacts": [
            {"path": path.relative_to(output).as_posix(), "sha256": sha256_file(path), "bytes": path.stat().st_size}
            for path in sorted(generated_paths, key=lambda value: value.relative_to(output).as_posix())
        ],
    }
    write_json(output / "manifest.json", manifest)
    print(str(report_path))
    return 0


def main() -> int:
    args = parse_args()
    try:
        return build(args)
    except ValidationFailure as exc:
        for error in exc.errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
