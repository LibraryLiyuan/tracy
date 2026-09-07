import unittest

from tools.n30_acceptance.query_anchors import require_anchors, compare_fields, relation_membership, resolve_gpu_frame_alias


class QueryAnchorTests(unittest.TestCase):
    def test_gpu_frame_index_is_not_correlated_frame_identity(self):
        expected = 562949953421313
        records = [{"frame_id": expected, "domain_id": 4, "domain_index": 617}]
        self.assertEqual(resolve_gpu_frame_alias(records, 617), expected)

    def test_frame_alias_requires_unique_explicit_gpu_memory_domain_record(self):
        for records in ([], [{"frame_id": 617, "domain_id": 1, "domain_index": 617}],
                        [{"frame_id": 1, "domain_id": 4, "domain_index": 617},
                         {"frame_id": 2, "domain_id": 4, "domain_index": 617}]):
            with self.assertRaises(ValueError):
                resolve_gpu_frame_alias(records, 617)

    def test_anchor_generation_must_match_pinned_session(self):
        with self.assertRaisesRegex(ValueError, "generation"):
            require_anchors({"schema": 1, "generation": "old"}, "current")

    def test_empty_anchor_output_must_not_pass(self):
        with self.assertRaisesRegex(ValueError, "shards"):
            require_anchors({"schema": 1, "generation": "current"}, "current")

    def test_integer_string_equivalence_without_float_rounding(self):
        values = compare_fields("resource", {"id": 9007199254740993}, {"id": "9007199254740993"})
        self.assertEqual(values[0].status.value, "PASS")
        self.assertEqual(values[0].delta, 0)

    def test_missing_field_is_failure_not_zero(self):
        values = compare_fields("resource", {"bytes": 0}, {})
        self.assertEqual(values[0].status.value, "FAIL")

    def test_relation_is_proven_only_by_exact_id_membership(self):
        self.assertTrue(relation_membership([{"resource_id": "7"}], "resource_id", 7))
        self.assertFalse(relation_membership([{"resource_id": "8", "name": "same"}], "resource_id", 7))


if __name__ == "__main__":
    unittest.main()
