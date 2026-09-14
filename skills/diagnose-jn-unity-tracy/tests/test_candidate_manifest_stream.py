"""Exercise the report's candidate input, not Trace parsing or statistics."""
import copy
import hashlib
import json
import sys
import tempfile
import tracemalloc
import unittest
from pathlib import Path

from tests.test_query_native_report_contract import query_native_analysis, load_contract

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))


class CandidateManifestStreamTests(unittest.TestCase):
    def package(self, root, manifest, rows=None):
        data = root / "candidate-records.ndjson"
        count = 0
        with data.open("wb") as out:
            for item in rows if rows is not None else manifest["candidates"]:
                out.write((json.dumps(item, ensure_ascii=False, sort_keys=True) + "\n").encode())
                count += 1
        descriptor = copy.deepcopy(manifest)
        descriptor.pop("candidates")
        descriptor["candidate_records"] = {
            "encoding": "ndjson-v1", "path": data.name,
            "sha256": hashlib.sha256(data.read_bytes()).hexdigest(),
            "count": count,
        }
        path = root / "candidate-manifest.json"
        path.write_text(json.dumps(descriptor), encoding="utf-8")
        return path, data, descriptor

    def test_streamed_validation_matches_inline_and_catches_numeric_mutation(self):
        from candidate_manifest import load_candidate_manifest
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            _, _, _, analysis, manifest = query_native_analysis(root)
            path, _, _ = self.package(root, manifest)
            streamed = load_candidate_manifest(path)
            self.assertEqual([], load_contract().validate_query_native_analysis(analysis, streamed, json.loads((root/"evidence-manifest.json").read_text()), root))
            analysis["candidate_findings"][0]["query_numeric_facts"][0]["observed_value"] = "999"
            self.assertTrue(any("query_numeric_facts" in e for e in
                load_contract().validate_query_native_analysis(analysis, streamed, json.loads((root/"evidence-manifest.json").read_text()), root)))

    def test_late_corruption_count_mismatch_duplicate_and_path_escape_fail(self):
        from candidate_manifest import load_candidate_manifest
        from report_common import ValidationFailure
        for failure in ("sha", "count", "duplicate", "escape", "dual"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                _, _, _, analysis, manifest = query_native_analysis(root)
                rows = manifest["candidates"] * (2 if failure == "duplicate" else 1)
                path, data, descriptor = self.package(root, manifest, rows)
                if failure == "sha":
                    data.write_bytes(data.read_bytes() + b" \n")
                elif failure == "count":
                    descriptor["candidate_records"]["count"] += 1
                elif failure == "escape":
                    descriptor["candidate_records"]["path"] = "../outside.ndjson"
                elif failure == "dual":
                    descriptor["candidates"] = []
                path.write_text(json.dumps(descriptor), encoding="utf-8")
                try:
                    errors = load_contract().validate_query_native_analysis(analysis, load_candidate_manifest(path))
                except ValidationFailure:
                    errors = ["input rejected"]
                self.assertTrue(errors, failure + " was silently accepted")

    def test_unselected_payload_is_not_materialized_and_required_cannot_hide(self):
        from candidate_manifest import load_candidate_manifest
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            _, _, _, analysis, manifest = query_native_analysis(root)
            count = 384
            def rows():
                yield manifest["candidates"][0]
                for i in range(count):
                    yield {"candidate_id": f"other-{i}", "priority": "P2", "domain": "cpu",
                           "selected": i == count - 1, "triggers": [], "unused_detail": "x" * 131072}
            path, _, _ = self.package(root, manifest, rows())
            analysis["investigation_backlog"] = [{"candidate_id": f"other-{i}"} for i in range(count)]
            analysis["query_scan"]["coverage"].update(candidate_total=count + 1, required_total=2,
                required_completed=1, investigated_total=1, uninvestigated_total=count)
            tracemalloc.start()
            try:
                errors = load_contract().validate_query_native_analysis(analysis, load_candidate_manifest(path))
                _, peak = tracemalloc.get_traced_memory()
            finally:
                tracemalloc.stop()
            self.assertTrue(any("other-383 has no completed investigation" in e for e in errors))
            self.assertLess(peak, 4 * 1024 * 1024, "report retained all unselected candidate payloads")

    def test_copy_preserves_manifest_and_payload_bytes_and_portability(self):
        from candidate_manifest import load_candidate_manifest, copy_candidate_manifest
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            _, _, _, analysis, manifest = query_native_analysis(root)
            path, data, _ = self.package(root, manifest)
            output = root / "report"
            output.mkdir()
            copied = copy_candidate_manifest(path, output)
            self.assertEqual(path.read_bytes(), (output / path.name).read_bytes())
            self.assertEqual(data.read_bytes(), (output / data.name).read_bytes())
            self.assertEqual({path.name, data.name}, {p.name for p in copied})
            self.assertEqual([], load_contract().validate_query_native_analysis(
                analysis, load_candidate_manifest(output / path.name), json.loads((root/"evidence-manifest.json").read_text()), root))

    def test_numeric_cli_and_report_accept_streamed_manifest(self):
        import subprocess
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            analysis_path, evidence_path, _, analysis, manifest = query_native_analysis(root)
            path, _, _ = self.package(root, manifest)
            analysis["query_scan"]["candidate_manifest_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
            analysis_path.write_text(json.dumps(analysis), encoding="utf-8")
            numeric = subprocess.run([sys.executable, str(SCRIPTS / "manage-analysis-state.py"),
                "validate-numerics", "--analysis", str(analysis_path), "--candidate-manifest", str(path)],
                capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(0, numeric.returncode, numeric.stdout + numeric.stderr)
            report = subprocess.run([sys.executable, str(SCRIPTS / "build_report.py"), "--legacy-regression",
                "--analysis", str(analysis_path), "--evidence", str(evidence_path), "--candidates", str(path),
                "--output", str(root / "report"), "--deterministic", "--export-static"],
                capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(0, report.returncode, report.stdout + report.stderr)
            self.assertEqual(path.read_bytes(), (root / "report/candidate-manifest.json").read_bytes())

    def test_payload_cannot_overwrite_reserved_report_artifact(self):
        from candidate_manifest import copy_candidate_manifest
        from report_common import ValidationFailure
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            _, _, _, _, manifest = query_native_analysis(root)
            path, data, descriptor = self.package(root, manifest)
            protected = "Performance-Analysis-Report.md"
            data.rename(root / protected)
            descriptor["candidate_records"]["path"] = protected
            path.write_text(json.dumps(descriptor), encoding="utf-8")
            output = root / "report"
            output.mkdir()
            (output / protected).write_text("KEEP", encoding="utf-8")
            with self.assertRaises(ValidationFailure):
                copy_candidate_manifest(path, output)
            self.assertEqual("KEEP", (output / protected).read_text(encoding="utf-8"))

    def test_invalid_finding_type_returns_validation_error(self):
        with tempfile.TemporaryDirectory() as temp:
            _, _, _, analysis, manifest = query_native_analysis(Path(temp))
            analysis["candidate_findings"] = None
            self.assertTrue(any("must be an array" in e for e in
                load_contract().validate_query_native_analysis(analysis, manifest)))

    def test_old_report_contract_suite_runs_without_other_test_imports(self):
        import subprocess
        result = subprocess.run([sys.executable, "-B", "-m", "unittest",
            "tests.test_query_native_report_contract.QueryNativeReportContractTests.test_ai_numeric_rewrite_is_rejected"],
            cwd=SCRIPTS.parent, capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_candidate_read_failure_is_structured_at_all_report_entrypoints(self):
        import subprocess
        for failure in ('missing_manifest', 'late_checksum'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                ap, ep, _, analysis, manifest = query_native_analysis(root)
                path, data, _ = self.package(root, manifest)
                analysis['query_scan']['candidate_manifest_sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
                ap.write_text(json.dumps(analysis), encoding='utf-8')
                if failure == 'missing_manifest':
                    path.unlink()
                else:
                    data.write_bytes(data.read_bytes().replace(b'"priority": "P1"', b'"priority": "P2"'))
                commands = [
                    ['manage-analysis-state.py', 'validate-numerics', '--analysis', str(ap), '--candidate-manifest', str(path)],
                    ['validate_analysis_result.py', '--analysis', str(ap), '--evidence', str(ep), '--candidates', str(path)],
                    ['build_report.py', '--analysis', str(ap), '--evidence', str(ep), '--candidates', str(path), '--output', str(root/'report')],
                ]
                for command in commands:
                    with self.subTest(entry=command[0]):
                        result = subprocess.run([sys.executable, '-X', 'utf8', str(SCRIPTS/command[0]), *command[1:]],
                            capture_output=True, text=True, encoding='utf-8')
                        self.assertNotEqual(0, result.returncode)
                        self.assertNotIn('Traceback', result.stdout + result.stderr)
                self.assertFalse((root/'report/manifest.json').exists())


if __name__ == "__main__":
    unittest.main()
