"""Deterministic phase runner with durable state and explicit stop rules."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import sys
from typing import Any, Callable, Mapping, TextIO

from .command_executor import CommandExecutor
from .comparison_log import ComparisonRecorder
from .orchestrator import dependency_allows
from .state_store import PhaseStore
from .status import AcceptanceStatus


@dataclass(frozen=True)
class PhaseOutcome:
    phase: str
    status: AcceptanceStatus
    summary: str
    repair_direction: str = ""
    evidence: Mapping[str, Any] = field(default_factory=dict)

    def as_dict(self) -> dict[str, Any]:
        return {
            "phase": self.phase,
            "status": AcceptanceStatus(self.status).value,
            "summary": self.summary,
            "repair_direction": self.repair_direction,
            "evidence": dict(self.evidence),
        }


@dataclass
class RunContext:
    run_directory: Path
    profile: Mapping[str, Any]
    inputs: Any
    recorder: ComparisonRecorder
    commands: CommandExecutor
    phase_store: PhaseStore
    run_fingerprint: str
    golden: Mapping[str, Any] | None = None
    progress: Any = None
    validate_identity: Any = None


PhaseHandler = Callable[[RunContext], PhaseOutcome]


class AcceptanceRunner:
    def __init__(
        self,
        context: RunContext,
        *,
        handlers: Mapping[str, PhaseHandler],
        console: TextIO | None = None,
        resume: bool = False,
    ):
        self.context = context
        self.handlers = dict(handlers)
        self.console = console if console is not None else sys.stdout
        self.resume = resume

    @staticmethod
    def _blocking_predecessor(
        outcomes: list[PhaseOutcome], candidate: str
    ) -> PhaseOutcome | None:
        for outcome in outcomes:
            if outcome.evidence.get("dependency_skipped"):
                continue
            if not dependency_allows(outcome.phase, outcome.status, candidate):
                return outcome
        return None

    def _commit(self, outcome: PhaseOutcome, comparison_summary: Mapping[str, int]) -> None:
        evidence = {
            **dict(outcome.evidence),
            "summary": outcome.summary,
            "repair_direction": outcome.repair_direction,
            "comparison_summary": dict(comparison_summary),
        }
        self.context.phase_store.commit(
            phase=outcome.phase, status=outcome.status, evidence=evidence
        )

    def run(self, phases: tuple[str, ...]) -> list[PhaseOutcome]:
        outcomes: list[PhaseOutcome] = []
        for phase in phases:
            if self.context.progress is not None:
                self.context.progress.update(phase=phase, command="", metrics_path="")
            if self.context.validate_identity is not None and phase != "P9":
                try:
                    self.context.validate_identity()
                except Exception as error:
                    outcome = PhaseOutcome(phase, AcceptanceStatus.HARNESS_ERROR,
                        str(error), repair_direction="restore locked identities or create a new Run")
                    self._commit(outcome, {"total": 0, "passed": 0, "failed": 0})
                    outcomes.append(outcome)
                    print(f"[PHASE][IDENTITY-ERROR] {phase}: {error}", file=self.console, flush=True)
                    continue
            if self.resume and self.context.phase_store.is_reusable(phase):
                print(f"[PHASE][REUSE] {phase}", file=self.console, flush=True)
                record = self.context.phase_store.load(phase) or {}
                evidence = record.get("evidence")
                outcomes.append(
                    PhaseOutcome(
                        phase,
                        AcceptanceStatus.PASS,
                        "reused matching passed phase",
                        evidence=evidence if isinstance(evidence, Mapping) else {},
                    )
                )
                continue

            blocker = self._blocking_predecessor(outcomes, phase)
            if blocker is not None:
                outcome = PhaseOutcome(
                    phase,
                    AcceptanceStatus.BLOCKED_ENVIRONMENT,
                    f"dependency blocked by {blocker.phase} ({blocker.status.value})",
                    repair_direction=f"resolve {blocker.phase} before running {phase}",
                    evidence={"dependency_skipped": True},
                )
                self._commit(outcome, {"total": 0, "passed": 0, "failed": 0})
                outcomes.append(outcome)
                print(
                    f"[PHASE][SKIP] {phase} reason={outcome.summary}",
                    file=self.console,
                    flush=True,
                )
                continue

            print(f"[PHASE][START] {phase}", file=self.console, flush=True)
            checkpoint = self.context.recorder.checkpoint()
            handler = self.handlers.get(phase)
            try:
                if handler is None:
                    raise KeyError(f"phase handler is not registered: {phase}")
                outcome = handler(self.context)
                if outcome.phase != phase:
                    raise ValueError(
                        f"phase handler returned {outcome.phase!r}, expected {phase!r}"
                    )
            except Exception as error:
                outcome = PhaseOutcome(
                    phase,
                    AcceptanceStatus.HARNESS_ERROR,
                    f"{type(error).__name__}: {error}",
                    repair_direction="repair the acceptance harness; do not attribute this to N30",
                )
            comparison_summary = self.context.recorder.print_summary(
                phase, since=checkpoint
            )
            self._commit(outcome, comparison_summary)
            outcomes.append(outcome)
            print(
                f"[PHASE][END] {phase} status={outcome.status.value} "
                f"summary={outcome.summary}",
                file=self.console,
                flush=True,
            )
        return outcomes
