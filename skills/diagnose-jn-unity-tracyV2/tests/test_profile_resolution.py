import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


SKILL_ROOT = Path(__file__).resolve().parents[1]
RESOLVER_PATH = SKILL_ROOT / "scripts" / "resolve-analysis-profile.py"
PROFILE_PATH = SKILL_ROOT / "config" / "JNTracy.AnalysisProfile.example.yaml"


def load_resolver():
    spec = importlib.util.spec_from_file_location("jntracy_profile_resolver", RESOLVER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load profile resolver")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class AnalysisProfileResolutionTests(unittest.TestCase):
    def test_safe_yaml_and_normalized_query_result_are_persisted(self) -> None:
        resolver = load_resolver()
        parsed = resolver.load_profile_yaml(PROFILE_PATH)
        self.assertEqual(parsed["schema_version"], 1)
        self.assertEqual(parsed["frame_budget"]["frame_ms"], 16.666667)
        self.assertEqual(parsed["resource_budgets"]["gpu_memory"]["unit"], "GB")
        self.assertNotIn("gpu_pass_budgets", parsed)

        normalized = dict(parsed)
        normalized["resource_budgets"] = {
            "cpu_memory_bytes": 16_000_000_000,
            "cpu_memory_status": "provisional",
            "gpu_memory_bytes": 6_400_000_000,
            "gpu_memory_status": "fixed",
        }
        with tempfile.TemporaryDirectory() as directory:
            outputs = resolver.persist_resolved_profile(
                Path(directory), parsed, normalized, "a" * 64
            )
            self.assertEqual(
                json.loads(outputs["json"].read_text(encoding="utf-8")), normalized
            )
            round_trip = resolver.load_profile_yaml(outputs["yaml"])
            self.assertEqual(round_trip, normalized)

    def test_python_object_constructor_is_rejected(self) -> None:
        resolver = load_resolver()
        with tempfile.TemporaryDirectory() as directory:
            hostile = Path(directory) / "hostile.yaml"
            hostile.write_text(
                "!!python/object/apply:os.system ['echo unsafe']\n", encoding="utf-8"
            )
            with self.assertRaises(resolver.ProfileResolutionError):
                resolver.load_profile_yaml(hostile)

    def test_saved_validation_must_come_from_query_135_mcp(self) -> None:
        resolver = load_resolver()
        with tempfile.TemporaryDirectory() as directory:
            response = Path(directory) / "validation.json"
            response.write_text(
                json.dumps(
                    {
                        "schema_version": "1.35.0",
                        "ok": True,
                        "data": {
                            "valid": True,
                            "profile_identity": "a" * 64,
                            "normalized_profile": {"schema_version": 1},
                        },
                    }
                ),
                encoding="utf-8",
            )
            self.assertTrue(resolver.load_mcp_validation(response)["valid"])
            value = json.loads(response.read_text(encoding="utf-8"))
            value["schema_version"] = "1.34.0"
            response.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaises(resolver.ProfileResolutionError):
                resolver.load_mcp_validation(response)


if __name__ == "__main__":
    unittest.main()
