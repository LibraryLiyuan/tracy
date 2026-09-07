import io
import tempfile
import unittest
from pathlib import Path

from tools.n30_acceptance.acceptance_runner import (
    AcceptanceRunner,
    PhaseOutcome,
    RunContext,
)
from tools.n30_acceptance.command_executor import CommandExecutor
from tools.n30_acceptance.comparison_log import ComparisonRecorder
from tools.n30_acceptance.comparators import compare_exact
from tools.n30_acceptance.state_store import PhaseStore
from tools.n30_acceptance.status import AcceptanceStatus


class AcceptanceRunnerTests(unittest.TestCase):
    def test_dependency_skip_does_not_hide_read_only_audit(self):
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            context = self._context(Path(directory), console)
            called = []
            def audit(ctx):
                called.append("P5")
                return PhaseOutcome("P5", AcceptanceStatus.BLOCKED_ENVIRONMENT, "read-only evidence")
            outcomes = AcceptanceRunner(context, console=console, handlers={
                "P3": lambda ctx: PhaseOutcome("P3", AcceptanceStatus.FAIL_CORRECTNESS, "mismatch"),
                "P5": audit,
            }).run(("P3", "P4", "P5"))
            self.assertEqual(called, ["P5"])
            self.assertEqual(outcomes[1].status, AcceptanceStatus.BLOCKED_ENVIRONMENT)

    def _context(self, root: Path, console: io.StringIO) -> RunContext:
        recorder = ComparisonRecorder(root / "comparisons.ndjson", console=console)
        return RunContext(
            run_directory=root,
            profile={"phases": ["P0", "P1", "P9"]},
            inputs=object(),
            recorder=recorder,
            commands=CommandExecutor(root),
            phase_store=PhaseStore(root, "fingerprint"),
            run_fingerprint="fingerprint",
        )

    def test_runs_phases_in_order_and_prints_each_phase_comparison_summary(self):
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            context = self._context(Path(directory), console)
            observed = []

            def phase(name):
                def execute(ctx):
                    observed.append(name)
                    ctx.recorder.record(compare_exact(f"{name}.metric", 1, 1))
                    return PhaseOutcome(name, AcceptanceStatus.PASS, "ok")
                return execute

            runner = AcceptanceRunner(
                context,
                handlers={"P0": phase("P0"), "P1": phase("P1"), "P9": phase("P9")},
                console=console,
            )
            outcomes = runner.run(("P0", "P1", "P9"))
            self.assertEqual(observed, ["P0", "P1", "P9"])
            self.assertEqual([item.status for item in outcomes], [AcceptanceStatus.PASS] * 3)
            rendered = console.getvalue()
            self.assertIn("[PHASE][START] P0", rendered)
            self.assertIn("[COMPARE-SUMMARY][P0] total=1 passed=1 failed=0", rendered)
            self.assertIn("[PHASE][END] P9 status=PASS", rendered)

    def test_failed_identity_stops_dependent_phase_but_report_still_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            context = self._context(Path(directory), console)
            called = []

            def fail_p0(ctx):
                called.append("P0")
                return PhaseOutcome("P0", AcceptanceStatus.BLOCKED_ENVIRONMENT, "bad input")

            def should_not_run(ctx):
                called.append("P1")
                return PhaseOutcome("P1", AcceptanceStatus.PASS, "wrong")

            def report(ctx):
                called.append("P9")
                return PhaseOutcome("P9", AcceptanceStatus.PASS, "reported")

            runner = AcceptanceRunner(
                context,
                handlers={"P0": fail_p0, "P1": should_not_run, "P9": report},
                console=console,
            )
            outcomes = runner.run(("P0", "P1", "P9"))
            self.assertEqual(called, ["P0", "P9"])
            self.assertEqual(outcomes[1].status, AcceptanceStatus.BLOCKED_ENVIRONMENT)
            self.assertIn("dependency blocked by P0", outcomes[1].summary)

    def test_resume_reuses_only_matching_passed_phase(self):
        with tempfile.TemporaryDirectory() as directory:
            console = io.StringIO()
            context = self._context(Path(directory), console)
            context.phase_store.commit(
                phase="P0", status=AcceptanceStatus.PASS, evidence={"value": 7}
            )
            called = []

            def phase(ctx):
                called.append("P0")
                return PhaseOutcome("P0", AcceptanceStatus.PASS, "rerun")

            runner = AcceptanceRunner(
                context, handlers={"P0": phase}, console=console, resume=True
            )
            outcomes = runner.run(("P0",))
            self.assertEqual(called, [])
            self.assertEqual(outcomes[0].summary, "reused matching passed phase")
            self.assertIn("[PHASE][REUSE] P0", console.getvalue())


if __name__ == "__main__":
    unittest.main()
