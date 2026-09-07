import io
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from tools.n30_acceptance.comparison_log import ComparisonRecorder
from tools.n30_acceptance.phase_p3 import evaluate_short_differential, run_p3
from tools.n30_acceptance.status import AcceptanceStatus


def response(identifier, data, *, ok=True):
    value = {
        "protocol": "tracy-query/1",
        "schema_version": "1.34.0",
        "id": identifier,
        "ok": ok,
    }
    if ok:
        value.update({"data": data, "partial": False, "omitted_count": "0"})
    else:
        value["error"] = {"code": "CAPABILITY_UNAVAILABLE", "details": {"domain": identifier}}
    return value


class ShortDifferentialTests(unittest.TestCase):
    def test_p3_uses_offline_full_worker_not_live_socket_replay(self):
        # Catch an accidental live/TCP replay choice. Only the external process
        # boundary is replaced: real P3 directory and command construction run.
        class CommandCaptured(Exception):
            pass

        class CaptureCommand:
            command = None

            def run(self, label, command, **kwargs):
                self.command = command
                raise CommandCaptured()

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "input.tracy-stream").write_bytes(b"short stream fixture")
            commands = CaptureCommand()
            context = SimpleNamespace(
                run_directory=root / "run",
                inputs=SimpleNamespace(
                    build_capture=root / "capture-build",
                    build_query=root / "query-build",
                    worktree=root,
                    short_stream=root / "input.tracy-stream",
                ),
                profile={},
                commands=commands,
                recorder=ComparisonRecorder(root / "comparisons.ndjson", console=io.StringIO()),
            )
            with self.assertRaises(CommandCaptured):
                run_p3(context)
            command = commands.command
            self.assertEqual(Path(command[0]).name, "tracy-stream-convert-legacy.exe")
            self.assertEqual(command[command.index("--mode") + 1], "offline")
            self.assertEqual(Path(command[command.index("-o") + 1]).suffix, ".tracy")
            self.assertIn("--no-cache", command)

    def test_p3_copies_source_beside_snapshot_without_touching_original_mapping(self):
        class CommandCaptured(Exception):
            pass

        class CaptureCommand:
            command = None

            def run(self, label, command, **kwargs):
                self.command = command
                raise CommandCaptured()

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = root / "input.tracy-stream"
            original.write_bytes(b"independent source fixture")
            mapping = root / "input.tracy-stream.snapshot-map"
            mapping.write_text("preserve old mapping", encoding="utf-8")
            commands = CaptureCommand()
            console = io.StringIO()
            context = SimpleNamespace(
                run_directory=root / "run",
                inputs=SimpleNamespace(build_capture=root / "capture-build",
                    build_query=root / "query-build", worktree=root, short_stream=original),
                profile={}, commands=commands,
                recorder=ComparisonRecorder(root / "comparisons.ndjson", console=console),
            )
            with self.assertRaises(CommandCaptured):
                run_p3(context)
            command = commands.command
            source = Path(command[command.index("-i") + 1])
            output = Path(command[command.index("-o") + 1])
            self.assertEqual(source.parent, output.parent)
            self.assertNotEqual(source, original)
            self.assertEqual(source.read_bytes(), b"independent source fixture")
            self.assertEqual(mapping.read_text(encoding="utf-8"), "preserve old mapping")
            self.assertIn("P3.input.copy.sha256", console.getvalue())

    def test_failed_full_worker_does_not_start_session_or_query(self):
        class FailConversion:
            calls = 0

            def run(self, label, command, **kwargs):
                self.calls += 1
                return SimpleNamespace(exit_code=7, peak_private_bytes=1024)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = root / "input.tracy-stream"
            original.write_bytes(b"short stream fixture")
            commands = FailConversion()
            context = SimpleNamespace(
                run_directory=root / "run",
                inputs=SimpleNamespace(build_capture=root / "capture-build",
                    build_query=root / "query-build", worktree=root, short_stream=original),
                profile={}, commands=commands,
                recorder=ComparisonRecorder(root / "comparisons.ndjson", console=io.StringIO()),
            )
            outcome = run_p3(context)
            self.assertNotEqual(outcome.status, AcceptanceStatus.PASS)
            self.assertEqual(commands.calls, 1)

    def _responses(self, count="7"):
        return [
            response("counts", {"cpu_zones": count, "gpu_zones": "3"}),
            response("samples", {}, ok=False),
        ]

    def test_equal_worker_and_session_print_request_and_metric_comparisons(self):
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=console)
            outcome = evaluate_short_differential(
                self._responses(), self._responses(), recorder=recorder
            )
            self.assertEqual(outcome.status, AcceptanceStatus.PASS)
            rendered = console.getvalue()
            self.assertIn("P3.response.counts.ok", rendered)
            self.assertIn("P3.response.samples.error_code", rendered)
            self.assertIn("P3.metric.trace.counts.cpu_zones", rendered)
            self.assertIn("P3.response.counts.semantic_hash", rendered)

    def test_any_count_or_semantic_change_is_correctness_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = ComparisonRecorder(Path(directory) / "comparisons.ndjson", console=io.StringIO())
            outcome = evaluate_short_differential(
                self._responses("7"), self._responses("8"), recorder=recorder
            )
            self.assertEqual(outcome.status, AcceptanceStatus.FAIL_CORRECTNESS)
            self.assertGreater(len(outcome.evidence["failed_comparisons"]), 0)


if __name__ == "__main__":
    unittest.main()
