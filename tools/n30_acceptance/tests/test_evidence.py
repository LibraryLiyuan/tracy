import io
from pathlib import Path
import tempfile
import unittest


from tools.n30_acceptance.comparison_log import ComparisonRecorder
from tools.n30_acceptance.evidence import (
    compare_metric_maps,
    extract_acceptance_metrics,
    parse_converter_summary,
)


class EvidenceExtractionTests(unittest.TestCase):
    def test_extracts_counts_gpu_verifier_peak_and_quality(self):
        responses = [
            {
                "id": "counts",
                "data": {"cpu_zones": "11", "gpu_zones": "12"},
            },
            {
                "id": "gpu-catalog-validation",
                "data": {
                    "counts": {
                        "resources": "7",
                        "allocations": "6",
                        "logical_resources": "5",
                        "parts": "4",
                        "range_records": "3",
                        "relations": "2",
                        "views": "1",
                        "vg_records": "0",
                    },
                    "independent_verifier": {
                        "resources": "7",
                        "allocations": "6",
                        "passes": "9",
                        "ranges": "3",
                        "direct_member_count": "13",
                        "inclusive_member_count": "17",
                        "resource_pass_relations": "17",
                        "forward_relation_hash": "19",
                        "reverse_relation_hash": "19",
                        "report_hash": "23",
                        "mismatch_count": "0",
                        "source_catalog_record_count": "29",
                        "source_gpu_catalog_events": "31",
                        "source_payload_bytes": "37",
                        "source_record_hash": "41",
                        "source_reference_relation_count": "13",
                    },
                    "source_quality": {
                        "gpu_reference_identity_gaps": "43",
                        "gpu_resource_identity_gaps": "47",
                    },
                    "reason": "source_cpu_zone_clock_inversion:53;source_scheduling_gap:59;source_gpu_resource_identity_gap:47",
                    "unresolved_breakdown": {"converter_created": "0"},
                    "transport": {"checksum_failures": "0"},
                },
            },
            {
                "id": "gpu-memory-peak",
                "data": {
                    "engine_known_physical_peak_bytes": "61",
                    "peak_time_ns": "67",
                },
            },
            {
                "id": "capture-coverage",
                "data": {
                    "producers": [
                        {
                            "key": "gpu.catalog",
                            "counters": {"dropped": "71", "overflow": "73"},
                        }
                    ]
                },
            },
        ]
        metrics = extract_acceptance_metrics(responses)
        self.assertEqual(metrics["trace.counts.cpu_zones"], "11")
        self.assertEqual(metrics["gpu.catalog.resources"], "7")
        self.assertEqual(metrics["gpu.verifier.report_hash"], "23")
        self.assertEqual(metrics["gpu.memory.engine_known_physical_peak_bytes"], "61")
        self.assertEqual(metrics["quality.invalid_cpu_zone_timings"], "53")
        self.assertEqual(metrics["quality.scheduling_source_gaps"], "59")
        self.assertEqual(metrics["quality.gpu_catalog_dropped"], "71")

    def test_converter_summary_is_strict_and_exact(self):
        summary = parse_converter_summary(
            "generation=n30-fixture canonical_shards=402 indexed_records=2285222848 "
            "index_bytes=123 gpu_resources=1769168 gpu_passes=27375840\n"
        )
        self.assertEqual(summary["session.canonical_shards"], "402")
        self.assertEqual(summary["session.indexed_records"], "2285222848")


class MetricComparisonTests(unittest.TestCase):
    def test_missing_metric_is_printed_as_failure_and_all_items_continue(self):
        console = io.StringIO()
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(
                Path(directory) / "comparisons.ndjson", console=console
            )
            results = compare_metric_maps(
                {"present": "1", "missing": "2"},
                {"present": "1"},
                recorder,
            )
            self.assertEqual([result.status.value for result in results], ["FAIL", "PASS"])
            self.assertIn("[COMPARE] name=missing", console.getvalue())
            self.assertIn("actual=<missing>", console.getvalue())
            self.assertIn("[COMPARE] name=present", console.getvalue())
            self.assertIn("status=FAIL", console.getvalue())
            self.assertIn("status=PASS", console.getvalue())


if __name__ == "__main__":
    unittest.main()
