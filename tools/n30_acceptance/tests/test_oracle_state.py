import json
from pathlib import Path
import tempfile
import unittest


from tools.n30_acceptance.io_utils import sha256_file
from tools.n30_acceptance.oracle import (
    GoldenBindingError,
    load_golden,
    verify_file_identity,
)
from tools.n30_acceptance.state_store import (
    PhaseStore,
    compute_run_fingerprint,
)
from tools.n30_acceptance.status import AcceptanceStatus


class FileIdentityTests(unittest.TestCase):
    def test_sha256_and_size_are_verified_exactly(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source.bin"
            path.write_bytes(b"abc")
            identity = verify_file_identity(
                path,
                expected_size=3,
                expected_sha256=(
                    "ba7816bf8f01cfea414140de5dae2223"
                    "b00361a396177a9cb410ff61f20015ad"
                ),
            )
            self.assertEqual(identity.size, 3)
            self.assertEqual(
                identity.sha256,
                "ba7816bf8f01cfea414140de5dae2223"
                "b00361a396177a9cb410ff61f20015ad",
            )

    def test_identity_mismatch_is_not_a_warning(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source.bin"
            path.write_bytes(b"abc")
            with self.assertRaises(GoldenBindingError):
                verify_file_identity(path, 4, "0" * 64)


class GoldenTests(unittest.TestCase):
    def test_golden_requires_expected_schema_and_source_binding(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "golden.json"
            path.write_text(
                json.dumps(
                    {
                        "golden_schema": 1,
                        "query_schema": "1.34.0",
                        "session_schema": 1,
                        "source": {"size": "3", "sha256": "a" * 64},
                        "metrics": {"gpu.resources": "7"},
                    }
                ),
                encoding="utf-8",
            )
            golden = load_golden(
                path,
                expected_query_schema="1.34.0",
                expected_session_schema=1,
                expected_source_sha256="a" * 64,
            )
            self.assertEqual(golden.metrics["gpu.resources"], "7")

            with self.assertRaises(GoldenBindingError):
                load_golden(
                    path,
                    expected_query_schema="1.35.0",
                    expected_session_schema=1,
                    expected_source_sha256="a" * 64,
                )


class StateStoreTests(unittest.TestCase):
    def test_run_fingerprint_is_canonical_and_sensitive(self):
        left = compute_run_fingerprint(
            {"profile": {"b": 2, "a": 1}, "source": "abc"}
        )
        right = compute_run_fingerprint(
            {"source": "abc", "profile": {"a": 1, "b": 2}}
        )
        changed = compute_run_fingerprint(
            {"source": "def", "profile": {"a": 1, "b": 2}}
        )
        self.assertEqual(left, right)
        self.assertNotEqual(left, changed)

    def test_only_matching_passed_phase_is_reusable(self):
        with tempfile.TemporaryDirectory() as directory:
            store = PhaseStore(Path(directory), "fingerprint-a")
            store.commit(
                phase="P0",
                status=AcceptanceStatus.PASS,
                evidence={"identity": "locked"},
            )
            self.assertTrue(store.is_reusable("P0"))

            other = PhaseStore(Path(directory), "fingerprint-b")
            self.assertFalse(other.is_reusable("P0"))

            store.commit(
                phase="P1",
                status=AcceptanceStatus.FAIL_CORRECTNESS,
                evidence={"reason": "mismatch"},
            )
            self.assertFalse(store.is_reusable("P1"))


if __name__ == "__main__":
    unittest.main()
