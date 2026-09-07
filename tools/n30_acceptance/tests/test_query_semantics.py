import unittest

from tools.n30_acceptance.query_semantics import stable_response_hash


class QuerySemanticHashTests(unittest.TestCase):
    def test_quality_warnings_are_semantic_evidence(self):
        left = {"id": "status", "data": {}, "warnings": []}
        right = {"id": "status", "data": {}, "warnings": ["source_gap"]}
        self.assertNotEqual(stable_response_hash(left), stable_response_hash(right))

    def test_ignores_transport_budget_trace_envelope_and_cursor_identity(self):
        left = {
            "id": "zones",
            "ok": True,
            "schema_version": "1.34.0",
            "budget": {"consumed": {"cpu_ms": "1"}},
            "trace": {"fingerprint": "a", "source_kind": "snapshot"},
            "page": {"returned": 1, "next_cursor": "left"},
            "data": {"items": [{"ref": "tracy:v1:aaaaaaaaaaaaaaaa:cpu-zone:7", "time": "9"}]},
        }
        right = {
            "id": "zones",
            "ok": True,
            "schema_version": "1.34.0",
            "budget": {"consumed": {"cpu_ms": "99"}},
            "trace": {"fingerprint": "b", "source_kind": "session"},
            "page": {"returned": 1, "next_cursor": "right"},
            "data": {"items": [{"ref": "tracy:v1:bbbbbbbbbbbbbbbb:cpu-zone:7", "time": "9"}]},
        }
        self.assertEqual(stable_response_hash(left), stable_response_hash(right))

    def test_semantic_value_change_changes_hash(self):
        left = {"id": "counts", "ok": True, "data": {"cpu_zones": "7"}}
        right = {"id": "counts", "ok": True, "data": {"cpu_zones": "8"}}
        self.assertNotEqual(stable_response_hash(left), stable_response_hash(right))


if __name__ == "__main__":
    unittest.main()
