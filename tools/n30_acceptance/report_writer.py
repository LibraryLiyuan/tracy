"""Deterministic machine, JUnit and human-readable acceptance reports."""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
from typing import Any, Mapping
import xml.etree.ElementTree as ET

from .io_utils import atomic_write_bytes, atomic_write_json, sha256_file


@dataclass(frozen=True)
class ReportOutputs:
    result_json: Path
    metrics_json: Path
    comparison_json: Path
    junit_xml: Path
    markdown: Path
    evidence_sha256: Path
    rerun_command: Path | None


def _markdown_cell(value: Any) -> str:
    if isinstance(value, (dict, list, tuple)):
        rendered = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    else:
        rendered = str(value)
    return rendered.replace("|", "\\|").replace("\n", "<br>")


def _write_junit(path: Path, result: Mapping[str, Any]) -> None:
    phases = list(result.get("phases", []))
    failures = sum(1 for phase in phases if str(phase.get("status", "")).startswith("FAIL_"))
    errors = sum(
        1
        for phase in phases
        if phase.get("status") in {"BLOCKED_ENVIRONMENT", "HARNESS_ERROR"}
    )
    skipped = sum(1 for phase in phases if phase.get("status") == "DEFERRED_OUT_OF_SCOPE")
    suite = ET.Element(
        "testsuite",
        {
            "name": "N30A deterministic full acceptance",
            "tests": str(len(phases)),
            "failures": str(failures),
            "errors": str(errors),
            "skipped": str(skipped),
        },
    )
    for phase in phases:
        case = ET.SubElement(
            suite,
            "testcase",
            {"classname": "N30A", "name": str(phase.get("phase", "unknown"))},
        )
        status = phase.get("status")
        summary = str(phase.get("summary", ""))
        if isinstance(status, str) and status.startswith("FAIL_"):
            node = ET.SubElement(case, "failure", {"type": status})
            node.text = summary
        elif status in {"BLOCKED_ENVIRONMENT", "HARNESS_ERROR"}:
            node = ET.SubElement(case, "error", {"type": str(status)})
            node.text = summary
        elif status == "DEFERRED_OUT_OF_SCOPE":
            node = ET.SubElement(case, "skipped")
            node.text = summary
    payload = ET.tostring(suite, encoding="utf-8", xml_declaration=True)
    atomic_write_bytes(path, payload + b"\n")


def _write_markdown(path: Path, result: Mapping[str, Any]) -> None:
    lines = [
        "# N30A 确定性全量验收报告",
        "",
        f"最终结论：**{result.get('overall_status', 'UNKNOWN')}**",
        "",
        "## 范围与延期",
        "",
        "本次范围：" + ", ".join(str(value) for value in result.get("scope", [])),
        "",
        "延期项：" + ", ".join(str(value) for value in result.get("deferred", [])),
        "",
        "## 身份",
        "",
        "| 字段 | 值 |",
        "|---|---|",
    ]
    for key, value in sorted(dict(result.get("identity", {})).items()):
        lines.append(f"| {_markdown_cell(key)} | {_markdown_cell(value)} |")
    lines.extend(
        [
            "",
            "## 阶段结果",
            "",
            "| 阶段 | 状态 | 摘要 | 修复方向 |",
            "|---|---|---|---|",
        ]
    )
    for phase in result.get("phases", []):
        lines.append(
            "| {phase} | {status} | {summary} | {direction} |".format(
                phase=_markdown_cell(phase.get("phase", "")),
                status=_markdown_cell(phase.get("status", "")),
                summary=_markdown_cell(phase.get("summary", "")),
                direction=_markdown_cell(phase.get("repair_direction", "")),
            )
        )
    if result.get("missing_required_phases"):
        lines.extend(["", "尚未执行的必需阶段：" + ", ".join(result["missing_required_phases"])])
    groups = (
        ("全域计数与源端退化", ("P3.", "P5.")),
        ("Query 全域、分页、范围与跨域验证", ("P6.",)),
        ("取消、恢复与故障注入", ("P7.",)),
        ("Synthetic 时间跨度与密度增长", ("P8.",)),
    )
    comparisons = list(result.get("comparisons", []))
    for title, prefixes in groups:
        selected = [item for item in comparisons if str(item.get("name", "")).startswith(prefixes)]
        lines.extend(["", "## " + title, ""])
        if not selected:
            lines.append("未取得本域比较证据；不得解释为通过或数据为空。")
            continue
        failures = [item for item in selected if item.get("status") == "FAIL"]
        lines.append(f"比较 {len(selected)} 项，通过 {len(selected) - len(failures)} 项，失败 {len(failures)} 项。完整数值见下方逐项比较。")
        for item in failures:
            lines.append(f"- `{item.get('name')}`：期望 {_markdown_cell(item.get('expected'))}；实际 {_markdown_cell(item.get('actual'))}；差值 {_markdown_cell(item.get('delta'))}。")
    lines.extend(["", "## 进程时间与资源", "",
        "数值来自每次实际启动的进程树；内存为 Private Bytes，时间含该命令完整执行。", "",
        "| 命令 | 退出码 | 秒 | 峰值 MiB | 读字节 | 写字节 | 超时 |", "|---|---:|---:|---:|---:|---:|---|"])
    for command in result.get("metrics", {}).get("commands", []):
        lines.append(f"| {_markdown_cell(command.get('label'))} | {command.get('exit_code')} | "
            f"{int(command.get('elapsed_ns', 0)) / 1e9:.3f} | {int(command.get('peak_private_bytes', 0)) / 2**20:.3f} | "
            f"{command.get('read_bytes', 'unavailable')} | {command.get('write_bytes', 'unavailable')} | {command.get('timed_out', 'unavailable')} |")
    lines.extend(
        [
            "",
            "## 逐项比较",
            "",
            "| 比较项 | 状态 | 规则 | 期望值 | 实际值 | 差值 | 说明 |",
            "|---|---|---|---:|---:|---:|---|",
        ]
    )
    for item in result.get("comparisons", []):
        lines.append(
            "| {name} | {status} | {rule} | {expected} | {actual} | {delta} | {detail} |".format(
                **{key: _markdown_cell(item.get(key, "")) for key in (
                    "name", "status", "rule", "expected", "actual", "delta", "detail"
                )}
            )
        )
    lines.extend(
        [
            "",
            "## 语义比较固定排除项",
            "",
        ]
    )
    for value in result.get("comparison_exclusions", []):
        lines.append(f"- `{value}`")
    lines.extend(["", "Query 表示层的固定规范化（质量警告完整保留）：", ""])
    for value in result.get("semantic_projections", []):
        lines.append(f"- {value}")
    failures = [
        phase
        for phase in result.get("phases", [])
        if phase.get("status") != "PASS"
    ]
    lines.extend(["", "## 失败证据与修复方向", ""])
    if not failures:
        lines.append("无失败项。")
    else:
        for phase in failures:
            lines.append(
                f"- **{phase.get('phase', '')} / {phase.get('status', '')}**："
                f"{phase.get('summary', '')}；方向：{phase.get('repair_direction', '待定位')}"
            )
    lines.extend(["", "## 阶段证据位置", ""])
    for phase in result.get("phases", []):
        evidence = phase.get("evidence", {})
        lines.append(f"- {phase.get('phase')}: `{_markdown_cell(evidence)}`")
    if result.get("rerun_command"):
        lines.extend(["", "## 重现命令", "", "```text", str(result["rerun_command"]), "```"])
    atomic_write_bytes(path, ("\n".join(lines) + "\n").encode("utf-8"))


def write_final_reports(report_directory: Path, result: Mapping[str, Any]) -> ReportOutputs:
    report_directory = Path(report_directory)
    report_directory.mkdir(parents=True, exist_ok=True)
    result_json = report_directory / "acceptance-result.json"
    metrics_json = report_directory / "acceptance-metrics.json"
    comparison_json = report_directory / "acceptance-comparison.json"
    junit_xml = report_directory / "acceptance-junit.xml"
    markdown = report_directory / "N30A-Full-Acceptance-Report.zh-CN.md"
    evidence_sha256 = report_directory / "EVIDENCE-SHA256.txt"
    rerun_command = None

    atomic_write_json(result_json, dict(result))
    atomic_write_json(metrics_json, result.get("metrics", {}))
    atomic_write_json(
        comparison_json,
        {
            "comparisons": result.get("comparisons", []),
            "comparison_exclusions": result.get("comparison_exclusions", []),
        },
    )
    _write_junit(junit_xml, result)
    _write_markdown(markdown, result)
    if result.get("rerun_command"):
        rerun_command = report_directory / "rerun-command.txt"
        atomic_write_bytes(
            rerun_command,
            (str(result["rerun_command"]) + "\n").encode("utf-8"),
        )

    evidence_files = sorted(
        path
        for path in report_directory.iterdir()
        if path.is_file() and path.name != evidence_sha256.name
    )
    for name in ("raw", "phases"):
        folder = report_directory.parent / name
        if folder.is_dir():
            evidence_files.extend(sorted(path for path in folder.rglob("*") if path.is_file()))
    evidence_files.extend(sorted(path for path in report_directory.parent.iterdir()
                                 if path.is_file() and path.suffix in {".json", ".ndjson"}))
    manifest_lines = [
        f"{sha256_file(path)}  {Path(os.path.relpath(path, report_directory)).as_posix()}" for path in evidence_files
    ]
    atomic_write_bytes(
        evidence_sha256, ("\n".join(manifest_lines) + "\n").encode("utf-8")
    )
    return ReportOutputs(
        result_json=result_json,
        metrics_json=metrics_json,
        comparison_json=comparison_json,
        junit_xml=junit_xml,
        markdown=markdown,
        evidence_sha256=evidence_sha256,
        rerun_command=rerun_command,
    )
