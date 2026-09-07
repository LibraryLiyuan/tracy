import io
import tempfile
import unittest
from pathlib import Path

from tools.n30_acceptance.comparison_log import ComparisonRecorder
from tools.n30_acceptance.phase_p8 import (
    DensityObservation,
    DurationObservation,
    evaluate_synthetic_scale,
    parse_corpus_report,
)
from tools.n30_acceptance.status import AcceptanceStatus


class CorpusReportTests(unittest.TestCase):
    def test_parses_decimal_strings_without_float_coercion(self):
        report = parse_corpus_report(
            '{"schema":"1","mode":"duration","logical_duration_ns":"7200000000000",'
            '"maximum_frame_id":"4294967536"}'
        )
        self.assertEqual(report["schema"], 1)
        self.assertEqual(report["logical_duration_ns"], 7_200_000_000_000)
        self.assertEqual(report["maximum_frame_id"], 4_294_967_536)

    def test_rejects_duplicate_json_or_wrong_schema(self):
        with self.assertRaises(ValueError):
            parse_corpus_report('{"schema":1}\n{"schema":1}')
        with self.assertRaises(ValueError):
            parse_corpus_report('{"schema":2,"mode":"duration"}')


class SyntheticScaleEvaluationTests(unittest.TestCase):
    def test_last_frame_lost_is_a_correctness_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_synthetic_scale(
                self._duration(expected_maximum_frame_id=4_294_967_537),
                [self._density(1), self._density(2), self._density(4)],
                expected_duration_ns=7_200_000_000_000, minimum_shards=240,
                maximum_growth_numerator=5, maximum_growth_denominator=2,
                hard_private_bytes=16 * 1024**3, recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_CORRECTNESS)

    def _duration(self, **changes):
        values = dict(
            generator_exit_code=0,
            converter_exit_code=0,
            inspector_exit_code=0,
            logical_duration_ns=7_200_000_000_000,
            maximum_frame_id=4_294_967_536,
            data_shards=241,
            source_protocol_events=500,
            canonical_protocol_events=500,
            final_audit_complete=True,
            zone_cross_shard=True,
            job_cross_shard=True,
            allocation_cross_shard=True,
            resource_cross_shard=True,
            io_cross_shard=True,
            peak_private_bytes=512 * 1024 * 1024,
        )
        values.update(changes)
        return DurationObservation(**values)

    def _density(self, factor, **changes):
        values = dict(
            factor=factor,
            generator_exit_code=0,
            converter_exit_code=0,
            inspector_exit_code=0,
            payload_records=100_000 * factor,
            source_protocol_events=200_000 * factor,
            canonical_protocol_events=200_000 * factor,
            source_bytes=10_000_000 * factor,
            session_bytes=20_000_000 * factor,
            temporary_bytes=1_000_000,
            data_shards=factor,
            core_build_elapsed_ns=1_000_000_000 * factor,
            peak_private_bytes=500_000_000 + 10_000_000 * factor,
            final_audit_complete=True,
            core_gap_count=0,
        )
        values.update(changes)
        return DensityObservation(**values)

    def test_prints_every_duration_and_density_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=console)
            outcome = evaluate_synthetic_scale(
                self._duration(),
                [self._density(1), self._density(2), self._density(4)],
                expected_duration_ns=7_200_000_000_000,
                minimum_shards=240,
                maximum_growth_numerator=5,
                maximum_growth_denominator=2,
                hard_private_bytes=16 * 1024**3,
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.PASS)
            output = console.getvalue()
            self.assertIn("P8.duration.logical_duration_ns", output)
            self.assertIn("P8.duration.lifecycle.resource_cross_shard", output)
            self.assertIn("P8.density.4x.payload_records", output)
            self.assertIn("P8.density.2x_vs_1x.core_build_growth", output)
            self.assertIn("P8.density.4x_vs_1x.peak_memory_growth", output)
            self.assertIn("status=PASS", output)

    def test_semantic_or_audit_failure_is_correctness_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_synthetic_scale(
                self._duration(resource_cross_shard=False),
                [self._density(1), self._density(2), self._density(4, core_gap_count=1)],
                expected_duration_ns=7_200_000_000_000,
                minimum_shards=240,
                maximum_growth_numerator=5,
                maximum_growth_denominator=2,
                hard_private_bytes=16 * 1024**3,
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_CORRECTNESS)

    def test_superlinear_time_or_memory_is_performance_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_synthetic_scale(
                self._duration(),
                [
                    self._density(1),
                    self._density(2, core_build_elapsed_ns=3_000_000_000),
                    self._density(4, core_build_elapsed_ns=8_000_000_000,
                                  peak_private_bytes=2_000_000_001),
                ],
                expected_duration_ns=7_200_000_000_000,
                minimum_shards=240,
                maximum_growth_numerator=5,
                maximum_growth_denominator=2,
                hard_private_bytes=16 * 1024**3,
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_PERFORMANCE)

    def test_missing_density_factor_is_harness_error(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_synthetic_scale(
                self._duration(),
                [self._density(1), self._density(4)],
                expected_duration_ns=7_200_000_000_000,
                minimum_shards=240,
                maximum_growth_numerator=5,
                maximum_growth_denominator=2,
                hard_private_bytes=16 * 1024**3,
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.HARNESS_ERROR)

    def test_density_must_cross_enough_shards_to_measure_bounded_memory(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(
                Path(directory) / "comparisons.ndjson", console=io.StringIO()
            )
            outcome = evaluate_synthetic_scale(
                self._duration(),
                [self._density(1), self._density(2), self._density(4)],
                expected_duration_ns=7_200_000_000_000,
                minimum_shards=240,
                maximum_growth_numerator=5,
                maximum_growth_denominator=2,
                hard_private_bytes=16 * 1024**3,
                recorder=recorder,
                minimum_density_shards=2,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_CORRECTNESS)

    def test_zero_growth_measurement_is_explicit_performance_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=console)
            outcome = evaluate_synthetic_scale(
                self._duration(),
                [
                    self._density(1, core_build_elapsed_ns=0, peak_private_bytes=0),
                    self._density(2),
                    self._density(4),
                ],
                expected_duration_ns=7_200_000_000_000,
                minimum_shards=240,
                maximum_growth_numerator=5,
                maximum_growth_denominator=2,
                hard_private_bytes=16 * 1024**3,
                recorder=recorder,
            )
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_PERFORMANCE)
            self.assertIn("measurement unavailable", console.getvalue())


if __name__ == "__main__":
    unittest.main()
