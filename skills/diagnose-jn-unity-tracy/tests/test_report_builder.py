import base64
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SKILL_ROOT = Path(__file__).resolve().parents[1]
BUILD_REPORT = SKILL_ROOT / "scripts" / "build_report.py"
VALIDATE_RESULT = SKILL_ROOT / "scripts" / "validate_analysis_result.py"


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    return digest.hexdigest()


def valid_inputs(root: Path) -> tuple[Path, Path]:
    trace_sha = "a" * 64
    data_path = root / "evidence-data" / "timeline.json"
    write_json(
        data_path,
        {
            "trace_id": "trace-1",
            "trace_sha256": trace_sha,
            "frame_id": 42,
            "start_ns": 1_000_000,
            "end_ns": 18_000_000,
            "lanes": [
                {"lane_id": "main", "label": "Main Thread", "kind": "thread"},
                {"lane_id": "render", "label": "Render Thread", "kind": "thread"},
                {"lane_id": "gpu", "label": "GPU Direct", "kind": "queue"},
            ],
            "zones": [
                {
                    "zone_ref": "zone-player-frame",
                    "lane_id": "main",
                    "parent_ref": None,
                    "label": "Player.Frame",
                    "start_ns": 1_000_000,
                    "end_ns": 18_000_000,
                },
                {
                    "zone_ref": "zone-render-fence",
                    "lane_id": "render",
                    "parent_ref": None,
                    "label": "Render.SubmissionFence",
                    "start_ns": 5_000_000,
                    "end_ns": 18_000_000,
                },
                {
                    "zone_ref": "zone-tsr",
                    "lane_id": "gpu",
                    "parent_ref": None,
                    "label": "GPU.Direct/PostProcessing/TSR",
                    "start_ns": 7_000_000,
                    "end_ns": 17_000_000,
                },
            ],
            "relations": [
                {
                    "relation_ref": "rel-main-render",
                    "from_ref": "zone-player-frame",
                    "to_ref": "zone-render-fence",
                    "kind": "waits_render",
                },
                {
                    "relation_ref": "rel-render-gpu",
                    "from_ref": "zone-render-fence",
                    "to_ref": "zone-tsr",
                    "kind": "waits_gpu",
                },
            ],
        },
    )

    evidence = {
        "schema_version": 1,
        "trace": {"trace_id": "trace-1", "sha256": trace_sha},
        "evidence": [
            {
                "evidence_id": "EVD-TIMELINE-001",
                "trace_id": "trace-1",
                "trace_sha256": trace_sha,
                "type": "correlated_timeline",
                "mcp": {
                    "method": "frame.correlated_timeline",
                    "params": {"frame_id": 42},
                },
                "request": {"path": "evidence-data/request.json", "sha256": "b" * 64},
                "response": {"path": "evidence-data/timeline.json", "sha256": sha256(data_path)},
                "refs": {
                    "frames": [42],
                    "time_ranges_ns": [[1_000_000, 18_000_000]],
                    "threads": ["main"],
                    "queues": [],
                    "entities": ["zone-player-frame", "zone-render-fence", "zone-tsr", "rel-main-render", "rel-render-gpu"],
                },
                "partial": False,
                "truncated": False,
                "next_cursor": None,
                "attempt": 1,
                "quality": "complete",
                "data_path": "evidence-data/timeline.json",
            }
        ],
    }
    evidence_path = root / "evidence-manifest.json"
    write_json(evidence_path, evidence)

    analysis = {
        "schema_version": 2,
        "report_identity": {
            "report_id": "G01-SINGLE-TRACE",
            "title": "G01 单 Trace 性能诊断",
            "generated_at": "2026-08-30T00:00:00+08:00",
            "trace_id": "trace-1",
            "trace_sha256": trace_sha,
            "configuration": {
                "local_profile": {"path": "config/local-profile.json", "sha256": "1" * 64},
                "project_profile": {"path": "config/project-profile.json", "sha256": "2" * 64},
                "performance_budgets": {"path": "config/performance-budgets.json", "sha256": "3" * 64},
                "marker_attribution": {"path": "config/marker-attribution.json", "sha256": "4" * 64},
            },
        },
        "analysis_scope": {
            "mode": "entire_capture",
            "start_ns": 0,
            "end_ns": 20_000_000,
            "valid_frame_count": 1200,
            "excluded_frame_count": 0,
        },
        "trace_quality": {
            "status": "complete",
            "warnings": [],
            "partial_domains": [],
        },
        "frame_class_summary": [
            {
                "class": "RecurrentSpike",
                "count": 3,
                "ratio": 0.0025,
                "flags": ["BudgetMiss"],
            }
        ],
        "bottleneck_signatures": [
            {
                "id": "SIG-0001",
                "stable_key": "GPU.Direct/PostProcessing/TSR",
                "significance_reasons": ["critical_path", "recurrent"],
                "affected_frames": [42, 642, 1042],
                "affected_ranges": [],
                "frequency": {"count": 3, "per_minute": 9.0},
                "budget_debt": {"total_ms": 13.0, "per_minute_ms": 39.0},
                "severity": "high",
                "primary_limiter": "GpuCriticalPath",
                "secondary_contributors": ["RenderWaitsGpuFencePresent"],
                "representative_instances": [
                    {"role": "typical", "frame_id": 42, "evidence_refs": ["EVD-TIMELINE-001"]},
                    {"role": "worst", "frame_id": 1042, "evidence_refs": ["EVD-TIMELINE-001"]},
                    {"role": "repeated", "frame_id": 642, "evidence_refs": ["EVD-TIMELINE-001"]},
                ],
                "causal_chain": [
                    {"relation_ref": "rel-main-render", "from_ref": "zone-player-frame", "from": "Main", "relation": "waits_render", "to_ref": "zone-render-fence", "to": "Render"},
                    {"relation_ref": "rel-render-gpu", "from_ref": "zone-render-fence", "from": "Render", "relation": "waits_gpu", "to_ref": "zone-tsr", "to": "GPU.Direct/TSR"},
                ],
                "hypothesis_ledger": [
                    {
                        "hypothesis": "TSR 位于帧完成关键路径",
                        "required_evidence": ["correlated_timeline"],
                        "mcp_queries": ["frame.correlated_timeline"],
                        "supporting_evidence": ["EVD-TIMELINE-001"],
                        "contradicting_evidence": [],
                        "status": "Supported",
                    }
                ],
                "deepest_level": "L6",
                "evidence_refs": ["EVD-TIMELINE-001"],
                "stack_source_provenance": ["MarkerOnly"],
                "supported_claims": ["TSR 控制这组帧的完成时间"],
                "unsupported_claims": ["像素着色器带宽受限"],
                "optimization_direction": ["降低 TSR 输入分辨率或检查该 Pass 的工作量"],
                "functional_risk": ["可能影响抗锯齿稳定性与画面清晰度"],
                "theoretical_upper_bound": {"value_ms": 10.0, "basis": "critical_path_duration"},
                "retest_metrics": ["TSR GPU duration P95", "Player.Frame P95"],
                "confidence": "high",
                "conclusion_status": "Supported",
                "evidence_gap": None,
            }
        ],
        "evidence_cards": [
            {
                "id": "CARD-SIG-0001",
                "signature_id": "SIG-0001",
                "title": "TSR recurrent GPU pressure",
                "evidence_refs": ["EVD-TIMELINE-001"],
                "visuals": ["timeline", "causal_chain"],
            }
        ],
        "domain_health": [
            {"domain": "GPU", "status": "pressure", "evidence_refs": ["EVD-TIMELINE-001"]}
        ],
        "cpu_memory_findings": [],
        "gpu_memory_findings": [],
        "tracy_findings": {
            "total_capture_overhead": "NotMeasuredSingleTrace",
            "runtime_findings": [],
            "analysis_findings": [],
        },
        "evidence_gaps": [
            {
                "id": "GAP-HW-COUNTERS",
                "reason": "HardwareCountersUnavailable",
                "minimum_additional_evidence": ["PIX 或厂商硬件计数器捕获"],
            }
        ],
        "query_audit_refs": ["EVD-TIMELINE-001"],
        "report_versions": {
            "analysis_result_schema": 2,
            "evidence_manifest_schema": 1,
            "report_builder": "1.1.1",
            "template": "1.1.1",
            "analysis_workflow": "1.1.1",
        },
    }
    analysis_path = root / "analysis-result.json"
    write_json(analysis_path, analysis)
    return analysis_path, evidence_path


class ReportBuilderTests(unittest.TestCase):
    def run_builder(self, analysis: Path, evidence: Path, output: Path) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                str(BUILD_REPORT),
                "--analysis",
                str(analysis),
                "--evidence",
                str(evidence),
                "--output",
                str(output),
                "--deterministic",
                "--export-static",
                "--html",
                "--standalone",
            ],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_g01_fixture_preserves_partial_query_boundary(self) -> None:
        fixture = json.loads((SKILL_ROOT / "tests" / "fixtures" / "g01-regression" / "evidence-data" / "frame-explain-527.json").read_text(encoding="utf-8"))
        self.assertTrue(fixture["trace"]["complete"])
        self.assertFalse(fixture["quality"]["complete"])
        self.assertTrue(fixture["quality"]["truncated"])
        self.assertGreater(fixture["quality"]["omitted_nodes"], 0)
        self.assertIn("cannot alone prove", fixture["contract"])

    def test_g01_real_evidence_regression_builds_with_explicit_gap(self) -> None:
        fixture_root = SKILL_ROOT / "tests" / "fixtures" / "g01-regression"
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "G01-AnalysisReport"
            result = self.run_builder(fixture_root / "analysis-result.json", fixture_root / "evidence-manifest.json", output)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = (output / "Performance-Analysis-Report.md").read_text(encoding="utf-8")
            svg_path = output / "visuals" / "assets" / "static-previews" / "signature-SIG-G01-UNRESOLVED-LONG-FRAME-timeline.svg"
            causal_path = output / "visuals" / "assets" / "static-previews" / "signature-SIG-G01-UNRESOLVED-LONG-FRAME-causal.svg"
            self.assertIn("UnresolvedWaitChain", report)
            self.assertIn("401.864731 ms", report)
            self.assertFalse(svg_path.exists())
            self.assertFalse(causal_path.exists())
            self.assertIn("静态 Timeline：未生成", report)
            self.assertIn("因果链图：未生成", report)
            self.assertNotIn("Primary Limiter：`GpuCriticalPath`", report)

    def test_nested_timeline_zones_use_non_overlapping_rows_and_bounded_labels(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            timeline = root / "evidence-data" / "timeline.json"
            timeline_value = json.loads(timeline.read_text(encoding="utf-8"))
            long_label = "PostLateUpdate.FinishFrameRendering.With.A.Very.Long.Diagnostic.Marker.Name"
            timeline_value["lanes"] = [{"lane_id": "main", "label": "Main Thread", "kind": "thread"}]
            timeline_value["zones"] = [
                {
                    "zone_ref": "zone-player-frame",
                    "lane_id": "main",
                    "parent_ref": None,
                    "label": "Main.PlayerLoop",
                    "start_ns": 1_000_000,
                    "end_ns": 18_000_000,
                },
                {
                    "zone_ref": "zone-render-fence",
                    "lane_id": "main",
                    "parent_ref": "zone-player-frame",
                    "label": long_label,
                    "start_ns": 2_000_000,
                    "end_ns": 17_000_000,
                },
                {
                    "zone_ref": "zone-tsr",
                    "lane_id": "main",
                    "parent_ref": "zone-render-fence",
                    "label": "JN.Direct/RenderPipeline.HDWorlds",
                    "start_ns": 3_000_000,
                    "end_ns": 16_000_000,
                },
            ]
            write_json(timeline, timeline_value)
            manifest = json.loads(evidence.read_text(encoding="utf-8"))
            manifest["evidence"][0]["response"]["sha256"] = sha256(timeline)
            write_json(evidence, manifest)

            output = root / "report"
            result = self.run_builder(analysis, evidence, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            svg = (output / "visuals" / "assets" / "static-previews" / "signature-SIG-0001-timeline.svg").read_text(encoding="utf-8")
            matches = list(
                __import__("re").finditer(
                    r'<g data-zone-ref="[^"]+"><rect x="[^"]+" y="([0-9.]+)" width="[^"]+" height="([0-9.]+)"',
                    svg,
                )
            )
            intervals = sorted((float(match.group(1)), float(match.group(1)) + float(match.group(2))) for match in matches)
            self.assertEqual(len(intervals), 3)
            for current, following in zip(intervals, intervals[1:]):
                self.assertLessEqual(current[1], following[0])
            self.assertIn(f"<title>{long_label}</title>", svg)
            self.assertNotIn(f">{long_label}</text>", svg)

    def test_causal_chain_wraps_long_node_labels(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            value = json.loads(analysis.read_text(encoding="utf-8"))
            value["bottleneck_signatures"][0]["causal_chain"] = [
                {
                    "relation_ref": "rel-main-render",
                    "from_ref": "zone-player-frame",
                    "from": "Main.PlayerLoop.With.A.Very.Long.Display.Name",
                    "relation": "waits_render",
                    "to_ref": "zone-render-fence",
                    "to": "PostLateUpdate.FinishFrameRendering.With.A.Very.Long.Display.Name",
                }
            ]
            write_json(analysis, value)
            output = root / "report"
            result = self.run_builder(analysis, evidence, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            svg = (output / "visuals" / "assets" / "static-previews" / "signature-SIG-0001-causal.svg").read_text(encoding="utf-8")
            self.assertIn("<tspan", svg)
            self.assertIn("Main.PlayerLoop.With.A.Very.Long.Display.Name</title>", svg)
            self.assertNotIn(">Main.PlayerLoop.With.A.Very.Long.Display.Name</text>", svg)

    def test_single_timestamp_memory_evidence_renders_named_snapshot_bars(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            snapshot_path = root / "evidence-data" / "memory-snapshot.json"
            write_json(
                snapshot_path,
                {
                    "trace_id": "trace-1",
                    "trace_sha256": "a" * 64,
                    "quality": "SnapshotOnly_PeakTimestampUnavailable",
                    "series": [
                        {"name": "DXGI Local Usage", "points": [{"time_ns": 20_000_000, "value_bytes": 5_944_037_376}]},
                        {"name": "GPU Local Threshold", "points": [{"time_ns": 20_000_000, "value_bytes": 6_400_000_000}]},
                        {"name": "Engine Physical Peak", "points": [{"time_ns": 20_000_000, "value_bytes": 4_509_990_912}]},
                    ],
                },
            )
            manifest = json.loads(evidence.read_text(encoding="utf-8"))
            manifest["evidence"].append(
                {
                    "evidence_id": "EVD-MEMORY-SNAPSHOT",
                    "trace_id": "trace-1",
                    "trace_sha256": "a" * 64,
                    "type": "memory_timeline",
                    "mcp": {"method": "memory.gpu.summary", "params": {}},
                    "request": {"path": "evidence-data/memory-snapshot.request.json", "sha256": "c" * 64},
                    "response": {"path": "evidence-data/memory-snapshot.json", "sha256": sha256(snapshot_path)},
                    "refs": {"frames": [], "time_ranges_ns": [], "threads": [], "queues": [], "entities": []},
                    "partial": False,
                    "truncated": False,
                    "next_cursor": None,
                    "attempt": 1,
                    "quality": "complete",
                    "data_path": "evidence-data/memory-snapshot.json",
                }
            )
            write_json(evidence, manifest)
            value = json.loads(analysis.read_text(encoding="utf-8"))
            value["bottleneck_signatures"][0]["evidence_refs"].append("EVD-MEMORY-SNAPSHOT")
            value["evidence_cards"][0]["evidence_refs"].append("EVD-MEMORY-SNAPSHOT")
            write_json(analysis, value)

            output = root / "report"
            result = self.run_builder(analysis, evidence, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            svg = (output / "visuals" / "assets" / "static-previews" / "signature-SIG-0001-memory.svg").read_text(encoding="utf-8")
            self.assertIn('data-chart-kind="snapshot-bars"', svg)
            self.assertIn("DXGI Local Usage", svg)
            self.assertIn("GPU Local Threshold", svg)
            self.assertIn("Engine Physical Peak", svg)

    def test_deterministic_report_build(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            output_a = root / "report-a"
            output_b = root / "report-b"
            result_a = self.run_builder(analysis, evidence, output_a)
            result_b = self.run_builder(analysis, evidence, output_b)
            self.assertEqual(result_a.returncode, 0, result_a.stderr)
            self.assertEqual(result_b.returncode, 0, result_b.stderr)

            required = [
                "Performance-Analysis-Report.md",
                "Analysis-Process-and-Query-Audit.md",
                "Evidence-Index.md",
                "analysis-result.json",
                "evidence-manifest.json",
                "manifest.json",
                "report-validation.json",
                "visuals/index.html",
                "visuals/standalone.html",
                "visuals/assets/static-previews/signature-SIG-0001-timeline.svg",
                "visuals/assets/static-previews/signature-SIG-0001-causal.svg",
            ]
            for relative in required:
                self.assertTrue((output_a / relative).is_file(), relative)
                self.assertEqual(sha256(output_a / relative), sha256(output_b / relative), relative)

            report = (output_a / "Performance-Analysis-Report.md").read_text(encoding="utf-8")
            html = (output_a / "visuals" / "index.html").read_text(encoding="utf-8")
            self.assertIn("GPU.Direct/PostProcessing/TSR", report)
            self.assertIn("TotalCaptureOverhead = NotMeasuredSingleTrace", report)
            self.assertIn("visuals/index.html#signature-SIG-0001", report)
            self.assertIn('id="signature-SIG-0001"', html)
            self.assertNotIn("https://", html)

    def test_validate_only_requires_no_output_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            result = subprocess.run(
                [
                    sys.executable,
                    str(BUILD_REPORT),
                    "--analysis",
                    str(analysis),
                    "--evidence",
                    str(evidence),
                    "--validate-only",
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('"passed":true', result.stdout)

    def test_default_report_does_not_generate_html(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            output = root / "report"
            result = subprocess.run(
                [
                    sys.executable,
                    str(BUILD_REPORT),
                    "--analysis",
                    str(analysis),
                    "--evidence",
                    str(evidence),
                    "--output",
                    str(output),
                    "--deterministic",
                    "--export-static",
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            report = (output / "Performance-Analysis-Report.md").read_text(encoding="utf-8")
            self.assertFalse((output / "visuals" / "index.html").exists())
            self.assertFalse((output / "visuals" / "standalone.html").exists())
            self.assertNotIn("打开 HTML Evidence", report)

    def test_new_explicit_tracy_and_cpu_gpu_terminology_is_valid(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            value = json.loads(analysis.read_text(encoding="utf-8"))
            signature = value["bottleneck_signatures"][0]
            signature["stable_key"] = "TracyRuntime/InstrumentationOverhead/RenderGraph.CreatePooledResource"
            signature["primary_limiter"] = "TracyRuntimeOverhead"
            write_json(analysis, value)
            result = subprocess.run(
                [sys.executable, str(VALIDATE_RESULT), "--analysis", str(analysis), "--evidence", str(evidence)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_legacy_ambiguous_terminology_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            value = json.loads(analysis.read_text(encoding="utf-8"))
            signature = value["bottleneck_signatures"][0]
            signature["stable_key"] = "Memory/GpuCriticalPath/DXGI.LocalCapacityPressure"
            signature["primary_limiter"] = "RenderActiveWork"
            write_json(analysis, value)
            result = subprocess.run(
                [sys.executable, str(VALIDATE_RESULT), "--analysis", str(analysis), "--evidence", str(evidence)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("ambiguous legacy terminology", result.stderr)

    def test_frame_image_requires_mcp_resources_read_png(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            image_path = root / "evidence-data" / "frame-42.png"
            image_path.write_bytes(base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="))
            image_meta_path = root / "evidence-data" / "frame-image.json"
            write_json(
                image_meta_path,
                {
                    "trace_id": "trace-1",
                    "trace_sha256": "a" * 64,
                    "frame_id": 42,
                    "image_path": "evidence-data/frame-42.png",
                    "image_sha256": sha256(image_path),
                },
            )
            manifest = json.loads(evidence.read_text(encoding="utf-8"))
            manifest["evidence"].append(
                {
                    "evidence_id": "EVD-FRAMEIMAGE-LOCAL-DECODER",
                    "trace_id": "trace-1",
                    "trace_sha256": "a" * 64,
                    "type": "frame_image",
                    "mcp": {"method": "frame_image.raw", "params": {"ref": "frame-image:0"}},
                    "request": {"path": "evidence-data/frame-image.request.json", "sha256": "c" * 64},
                    "response": {"path": "evidence-data/frame-image.json", "sha256": sha256(image_meta_path)},
                    "refs": {"frames": [42], "time_ranges_ns": [], "threads": [], "queues": [], "entities": []},
                    "partial": False,
                    "truncated": False,
                    "next_cursor": None,
                    "attempt": 1,
                    "quality": "complete",
                    "data_path": "evidence-data/frame-image.json",
                }
            )
            write_json(evidence, manifest)
            result = subprocess.run(
                [sys.executable, str(VALIDATE_RESULT), "--analysis", str(analysis), "--evidence", str(evidence)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("resources/read", result.stderr)

    def test_rejects_significant_signature_without_l6_or_gap(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            value = json.loads(analysis.read_text(encoding="utf-8"))
            value["bottleneck_signatures"][0]["deepest_level"] = "L4"
            value["bottleneck_signatures"][0]["evidence_gap"] = None
            write_json(analysis, value)
            result = subprocess.run(
                [sys.executable, str(VALIDATE_RESULT), "--analysis", str(analysis), "--evidence", str(evidence)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("L6", result.stderr)

    def test_rejects_cross_trace_and_reversed_timeline(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            manifest = json.loads(evidence.read_text(encoding="utf-8"))
            manifest["evidence"][0]["trace_sha256"] = "f" * 64
            write_json(evidence, manifest)
            result = subprocess.run(
                [sys.executable, str(VALIDATE_RESULT), "--analysis", str(analysis), "--evidence", str(evidence)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("trace_sha256", result.stderr)

            analysis, evidence = valid_inputs(root)
            timeline = root / "evidence-data" / "timeline.json"
            timeline_value = json.loads(timeline.read_text(encoding="utf-8"))
            timeline_value["zones"][1]["start_ns"] = 19_000_000
            timeline_value["zones"][1]["end_ns"] = 17_000_000
            write_json(timeline, timeline_value)
            manifest = json.loads(evidence.read_text(encoding="utf-8"))
            manifest["evidence"][0]["response"]["sha256"] = sha256(timeline)
            write_json(evidence, manifest)
            result = subprocess.run(
                [sys.executable, str(VALIDATE_RESULT), "--analysis", str(analysis), "--evidence", str(evidence)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("time reversal", result.stderr)

    def test_rejects_wait_conclusion_without_real_relation(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            value = json.loads(analysis.read_text(encoding="utf-8"))
            signature = value["bottleneck_signatures"][0]
            signature["primary_limiter"] = "MainWaitsRender"
            signature["causal_chain"] = []
            signature["evidence_gap"] = None
            write_json(analysis, value)
            result = subprocess.run(
                [sys.executable, str(VALIDATE_RESULT), "--analysis", str(analysis), "--evidence", str(evidence)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("wait conclusion", result.stderr)

    def test_rejects_resource_level_claim_when_catalog_is_invalid(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            value = json.loads(analysis.read_text(encoding="utf-8"))
            value["gpu_memory_findings"] = [
                {
                    "classification": "CapacityPressure",
                    "summary": "Texture X owns the peak allocation",
                    "resource_level": True,
                    "catalog_status": "invalid_core_gap",
                    "evidence_refs": ["EVD-TIMELINE-001"],
                }
            ]
            write_json(analysis, value)
            result = subprocess.run(
                [sys.executable, str(VALIDATE_RESULT), "--analysis", str(analysis), "--evidence", str(evidence)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("GPU Catalog is invalid", result.stderr)

    def test_untrusted_strings_are_escaped_and_partial_lod_is_disclosed(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            value = json.loads(analysis.read_text(encoding="utf-8"))
            value["report_identity"]["title"] = "<img src=x onerror=alert(1)>"
            value["bottleneck_signatures"][0]["stable_key"] = "GPU.Direct/PostProcessing/<img src=x onerror=alert(1)>"
            write_json(analysis, value)

            timeline = root / "evidence-data" / "timeline.json"
            timeline_value = json.loads(timeline.read_text(encoding="utf-8"))
            timeline_value["zones"][1]["label"] = "<img src=x onerror=alert(2)>"
            write_json(timeline, timeline_value)
            manifest = json.loads(evidence.read_text(encoding="utf-8"))
            manifest["evidence"][0]["response"]["sha256"] = sha256(timeline)
            manifest["evidence"][0]["partial"] = True
            manifest["evidence"][0]["quality"] = "partial"
            write_json(evidence, manifest)

            output = root / "report"
            result = self.run_builder(analysis, evidence, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = (output / "Performance-Analysis-Report.md").read_text(encoding="utf-8")
            html = (output / "visuals" / "index.html").read_text(encoding="utf-8")
            svg = (output / "visuals" / "assets" / "static-previews" / "signature-SIG-0001-timeline.svg").read_text(encoding="utf-8")
            shard = (output / "visuals" / "assets" / "data" / "signature-SIG-0001.js").read_text(encoding="utf-8")
            self.assertNotIn("<img src=x", report)
            self.assertNotIn("<img src=x", html)
            self.assertNotIn("<img src=x", svg)
            self.assertNotIn("<img src=x", shard)
            self.assertIn("&lt;img", report)
            self.assertIn("LOD: QueryPartial", svg)

    def test_memory_resource_and_frame_image_visuals_are_generated(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            trace_sha = "a" * 64
            memory_path = root / "evidence-data" / "memory.json"
            resource_path = root / "evidence-data" / "resources.json"
            image_meta_path = root / "evidence-data" / "frame-image.json"
            image_path = root / "evidence-data" / "frame-42.png"
            image_path.write_bytes(base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="))
            write_json(
                memory_path,
                {
                    "trace_id": "trace-1",
                    "trace_sha256": trace_sha,
                    "series": [
                        {
                            "label": "DXGI Local Usage",
                            "budget_bytes": 6_400_000_000,
                            "points": [
                                {"time_ns": 1_000_000, "value_bytes": 4_000_000_000},
                                {"time_ns": 18_000_000, "value_bytes": 4_500_000_000},
                            ],
                        }
                    ],
                },
            )
            write_json(
                resource_path,
                {
                    "trace_id": "trace-1",
                    "trace_sha256": trace_sha,
                    "nodes": [
                        {"ref": "pass-tsr", "kind": "pass", "label": "TSR"},
                        {"ref": "resource-color", "kind": "resource", "label": "FinalColor"},
                        {"ref": "allocation-1", "kind": "allocation", "label": "64 MiB"},
                    ],
                    "edges": [
                        {"relation_ref": "rel-pass-resource", "from_ref": "pass-tsr", "to_ref": "resource-color", "kind": "uses"},
                        {"relation_ref": "rel-resource-allocation", "from_ref": "resource-color", "to_ref": "allocation-1", "kind": "backed_by"},
                    ],
                },
            )
            write_json(
                image_meta_path,
                {
                    "trace_id": "trace-1",
                    "trace_sha256": trace_sha,
                    "frame_id": 42,
                    "source": "mcp_resources_read",
                    "resource_uri": "tracy://trace-1/frame-image/42",
                    "mime_type": "image/png",
                    "image_path": "evidence-data/frame-42.png",
                    "image_sha256": sha256(image_path),
                },
            )
            manifest = json.loads(evidence.read_text(encoding="utf-8"))
            additions = [
                ("EVD-MEMORY-001", "memory_timeline", memory_path),
                ("EVD-RESOURCE-001", "resource_relation_graph", resource_path),
                ("EVD-FRAMEIMAGE-001", "frame_image", image_meta_path),
            ]
            for evidence_id, evidence_type, data_path in additions:
                manifest["evidence"].append(
                    {
                        "evidence_id": evidence_id,
                        "trace_id": "trace-1",
                        "trace_sha256": trace_sha,
                        "type": evidence_type,
                        "mcp": (
                            {"method": "resources/read", "params": {"uri": "tracy://trace-1/frame-image/42"}}
                            if evidence_type == "frame_image"
                            else {"method": evidence_type, "params": {"frame_id": 42}}
                        ),
                        "request": {"path": f"evidence-data/{evidence_id}.request.json", "sha256": "c" * 64},
                        "response": {"path": f"evidence-data/{data_path.name}", "sha256": sha256(data_path)},
                        "refs": {"frames": [42], "time_ranges_ns": [[1_000_000, 18_000_000]], "threads": [], "queues": ["gpu"], "entities": []},
                        "partial": False,
                        "truncated": False,
                        "next_cursor": None,
                        "attempt": 1,
                        "quality": "complete",
                        "data_path": f"evidence-data/{data_path.name}",
                    }
                )
            write_json(evidence, manifest)
            value = json.loads(analysis.read_text(encoding="utf-8"))
            extra_refs = [item[0] for item in additions]
            value["bottleneck_signatures"][0]["evidence_refs"].extend(extra_refs)
            value["evidence_cards"][0]["evidence_refs"].extend(extra_refs)
            write_json(analysis, value)

            output = root / "report"
            result = self.run_builder(analysis, evidence, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            expected = [
                "visuals/assets/static-previews/signature-SIG-0001-memory.svg",
                "visuals/assets/static-previews/signature-SIG-0001-resources.svg",
                "visuals/assets/frame-images/signature-SIG-0001-frame-42.png",
            ]
            report = (output / "Performance-Analysis-Report.md").read_text(encoding="utf-8")
            for relative in expected:
                self.assertTrue((output / relative).is_file(), relative)
                self.assertIn(relative, report)

    def test_empty_resource_graph_is_disclosed_without_placeholder_svg(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis, evidence = valid_inputs(root)
            resource_path = root / "evidence-data" / "resources-empty.json"
            write_json(
                resource_path,
                {
                    "trace_id": "trace-1",
                    "trace_sha256": "a" * 64,
                    "nodes": [],
                    "edges": [],
                },
            )
            manifest = json.loads(evidence.read_text(encoding="utf-8"))
            manifest["evidence"].append(
                {
                    "evidence_id": "EVD-RESOURCE-EMPTY",
                    "trace_id": "trace-1",
                    "trace_sha256": "a" * 64,
                    "type": "resource_relation_graph",
                    "mcp": {"method": "gpu.resource.explain", "params": {"resource_id": 0}},
                    "request": {"path": "evidence-data/resource-empty.request.json", "sha256": "c" * 64},
                    "response": {"path": "evidence-data/resources-empty.json", "sha256": sha256(resource_path)},
                    "refs": {"frames": [42], "time_ranges_ns": [], "threads": [], "queues": [], "entities": []},
                    "partial": False,
                    "truncated": False,
                    "next_cursor": None,
                    "attempt": 1,
                    "quality": "complete",
                    "data_path": "evidence-data/resources-empty.json",
                }
            )
            write_json(evidence, manifest)
            value = json.loads(analysis.read_text(encoding="utf-8"))
            value["bottleneck_signatures"][0]["evidence_refs"].append("EVD-RESOURCE-EMPTY")
            value["evidence_cards"][0]["evidence_refs"].append("EVD-RESOURCE-EMPTY")
            write_json(analysis, value)

            output = root / "report"
            result = self.run_builder(analysis, evidence, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            resource_svg = output / "visuals" / "assets" / "static-previews" / "signature-SIG-0001-resources.svg"
            self.assertFalse(resource_svg.exists())
            report = (output / "Performance-Analysis-Report.md").read_text(encoding="utf-8")
            self.assertNotIn("signature-SIG-0001-resources.svg", report)


if __name__ == "__main__":
    unittest.main()
