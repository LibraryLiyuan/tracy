import unittest

from tools.n30_acceptance.session_artifacts import (
    parse_gpu_verifier_report,
    parse_scalar_manifest,
)


class ScalarManifestTests(unittest.TestCase):
    def test_parses_exact_integers_strings_and_stops_at_repeated_records(self):
        values = parse_scalar_manifest(
            'magic 7\ngeneration "n30-a"\ncomplete 1\nfile 0 "x"\nfile 1 "y"\n',
            stop_keys={"file"},
        )
        self.assertEqual(values, {"magic": 7, "generation": "n30-a", "complete": 1})

    def test_rejects_duplicate_scalar_and_malformed_lines(self):
        with self.assertRaises(ValueError):
            parse_scalar_manifest("schema 1\nschema 1\n")
        with self.assertRaises(ValueError):
            parse_scalar_manifest("schema 1 extra\n")


class GpuVerifierReportTests(unittest.TestCase):
    def test_maps_product_fields_to_golden_metric_keys(self):
        report = parse_gpu_verifier_report(
            "resourceCount 7\nallocationCount 6\npassCount 5\nrangeCount 4\n"
            "resourcePassRelationCount 3\nforwardRelationHash 2\nreverseRelationHash 2\n"
            "reportHash 11\nmismatchCount 0\ncomplete 1\nreason \"\"\n"
        )
        self.assertEqual(report.metrics["gpu.verifier.resources"], "7")
        self.assertEqual(report.metrics["gpu.verifier.report_hash"], "11")
        self.assertTrue(report.complete)
        self.assertEqual(report.reason, "")


if __name__ == "__main__":
    unittest.main()
