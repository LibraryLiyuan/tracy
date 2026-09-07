"""Command-line entry point for deterministic N30A full acceptance."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from typing import Any, Mapping, Sequence

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    __package__ = "tools.n30_acceptance"

from .acceptance_runner import AcceptanceRunner, PhaseOutcome, RunContext
from .command_executor import CommandExecutor
from .comparison_log import ComparisonRecorder
from .comparators import comparison_exclusions
from .comparators import compare_equal, ComparisonStatus
from .configuration import load_local_inputs
from .io_utils import atomic_write_json, load_json, sha256_file
from .oracle import load_golden
from .orchestrator import PHASE_ORDER, select_phases
from .phase_p0 import run_p0
from .phase_p1 import run_p1
from .phase_p2 import run_p2
from .phase_p3 import run_p3
from .phase_p4 import run_p4
from .phase_p5 import run_p5
from .phase_p6 import run_p6
from .phase_p7 import run_p7
from .phase_p8 import run_p8
from .report_writer import write_final_reports
from .progress import AcceptanceProgress
from .query_semantics import SEMANTIC_PROJECTIONS
from .state_store import PhaseStore, compute_run_fingerprint
from .status import AcceptanceStatus, aggregate_overall_status


def create_run_directory(
    output_root: Path, fingerprint: str, *, now: datetime | None = None
) -> Path:
    instant = now if now is not None else datetime.now(timezone.utc)
    if instant.tzinfo is None:
        raise ValueError("run timestamp must be timezone-aware")
    run_id = f"n30a-{instant.astimezone(timezone.utc):%Y%m%d-%H%M%S}-{fingerprint[:8]}"
    run_directory = Path(output_root) / "runs" / run_id
    run_directory.mkdir(parents=True, exist_ok=False)
    return run_directory


def load_comparison_records(path: Path) -> list[dict[str, Any]]:
    path = Path(path)
    if not path.is_file():
        return []
    records: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError(f"comparison line {line_number} is not an object")
            if value.get("sequence") != len(records) + 1:
                raise ValueError(f"comparison sequence gap at line {line_number}")
            records.append(value)
    return records


def build_final_result(
    *,
    identity: Mapping[str, Any],
    outcomes: Sequence[Mapping[str, Any]],
    comparisons: Sequence[Mapping[str, Any]],
    profile: Mapping[str, Any],
    rerun_command: str,
    metrics: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    statuses = [AcceptanceStatus(value["status"]) for value in outcomes]
    required = tuple(profile.get("phases", PHASE_ORDER))
    missing = [phase for phase in required if not any(value["phase"] == phase for value in outcomes)]
    overall = aggregate_overall_status(statuses).value
    unaccounted = [value.get("name", "unknown") for value in comparisons
                   if value.get("status") == "FAIL"] if overall == "PASSED" else []
    if overall == "PASSED" and (missing or unaccounted):
        overall = "INCOMPLETE"
    return {
        "overall_status": overall,
        "missing_required_phases": missing,
        "unattributed_failed_comparisons": unaccounted,
        "scope": ["N30A deterministic full Session acceptance"],
        "deferred": list(profile.get("deferred", [])),
        "identity": dict(identity),
        "phases": [dict(value) for value in outcomes],
        "comparisons": [dict(value) for value in comparisons],
        "comparison_exclusions": list(comparison_exclusions()),
        "semantic_projections": list(SEMANTIC_PROJECTIONS),
        "metrics": dict(metrics or {}),
        "rerun_command": rerun_command,
    }


def _git_value(worktree: Path, arguments: Sequence[str]) -> bytes:
    process = subprocess.run(
        ("git", "-c", f"safe.directory={worktree.as_posix()}", *arguments),
        cwd=str(worktree),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if process.returncode != 0:
        raise RuntimeError(
            f"git {' '.join(arguments)} failed: "
            f"{process.stderr.decode('utf-8', errors='replace')}"
        )
    return process.stdout


def _bootstrap_identity(
    *,
    profile_path: Path,
    inputs_path: Path,
    golden_path: Path,
    inputs,
) -> dict[str, Any]:
    head = _git_value(inputs.worktree, ("rev-parse", "HEAD")).decode().strip()
    tracked_diff = _git_value(
        inputs.worktree, ("diff", "--no-ext-diff", "--binary", "HEAD", "--")
    )
    source_stat = inputs.source_30m.stat()
    current = (inputs.existing_session_30m / "CURRENT").read_text(
        encoding="utf-8"
    ).strip()
    untracked = {}
    for raw in _git_value(inputs.worktree, ("ls-files", "--others", "--exclude-standard", "-z")).split(b"\0"):
        if not raw:
            continue
        relative = raw.decode("utf-8")
        if relative.split("/")[0].startswith("build-") or "/__pycache__/" in relative:
            continue
        untracked[relative] = sha256_file(inputs.worktree / relative)
    return {
        "git_head": head,
        "git_tracked_diff_sha256": hashlib.sha256(tracked_diff).hexdigest(),
        "git_untracked_source_sha256": hashlib.sha256(json.dumps(untracked, sort_keys=True).encode()).hexdigest(),
        "profile_sha256": sha256_file(profile_path),
        "inputs_sha256": sha256_file(inputs_path),
        "golden_sha256": sha256_file(golden_path),
        "source_path": str(inputs.source_30m),
        "source_size": str(source_stat.st_size),
        "source_mtime_ns": str(source_stat.st_mtime_ns),
        "existing_session_current": current,
        "existing_session_metadata_sha256": _session_metadata_fingerprint(inputs.existing_session_30m),
    }


def _session_metadata_fingerprint(session: Path) -> str:
    current = (session / "CURRENT").read_text(encoding="utf-8").strip()
    root = session / "generations" / current
    facts = []
    for path in sorted(root.rglob("*")):
        if path.is_file():
            stat = path.stat()
            facts.append((path.relative_to(root).as_posix(), stat.st_size, stat.st_mtime_ns))
    for name in ("CURRENT", "manifest"):
        path = session / name
        if path.is_file():
            facts.append((name, sha256_file(path)))
    return hashlib.sha256(json.dumps(facts, separators=(",", ":")).encode()).hexdigest()


def _report_phase(context: RunContext) -> PhaseOutcome:
    identity = load_json(context.run_directory / "environment.json")
    checks = [context.recorder.record(compare_equal("P9.source.sha256_unchanged",
        context.golden["source"]["sha256"].lower(), sha256_file(context.inputs.source_30m).lower())),
        context.recorder.record(compare_equal("P9.existing_session.file_metadata_unchanged",
            identity["existing_session_metadata_sha256"],
            _session_metadata_fingerprint(context.inputs.existing_session_30m)))]
    if context.validate_identity is not None:
        try:
            context.validate_identity()
        except Exception as error:
            return PhaseOutcome("P9", AcceptanceStatus.FAIL_INTEGRITY, str(error))
    failed = any(value.status == ComparisonStatus.FAIL for value in checks)
    return PhaseOutcome(
        "P9",
        AcceptanceStatus.FAIL_INTEGRITY if failed else AcceptanceStatus.PASS,
        "final machine, JUnit and Markdown reports are emitted after phase commit",
    )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", required=True, type=Path)
    parser.add_argument("--inputs", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--status", action="store_true")
    parser.add_argument("--resume", action="store_true")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--from-phase", choices=PHASE_ORDER)
    selection.add_argument("--rerun-phase", choices=PHASE_ORDER)
    return parser


def _status(output_root: Path) -> int:
    active_path = Path(output_root) / "active-run.json"
    if not active_path.is_file():
        print("[STATUS] no active N30A run", flush=True)
        return 2
    active = load_json(active_path)
    run_directory = Path(active["run_directory"])
    phase_directory = run_directory / "phases"
    phases = []
    if phase_directory.is_dir():
        for path in sorted(phase_directory.glob("*.json")):
            phases.append(load_json(path))
    print(
        json.dumps(
            {"active": active, "phases": phases},
            ensure_ascii=False,
            indent=2,
            sort_keys=True,
        ),
        flush=True,
    )
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if args.status:
        return _status(args.output)

    profile_path = args.profile.resolve()
    inputs_path = args.inputs.resolve()
    profile = load_json(profile_path)
    if not isinstance(profile, dict) or profile.get("profile_schema") != 1:
        raise ValueError("unsupported or malformed N30A profile")
    inputs = load_local_inputs(inputs_path)
    golden_path = (profile_path.parent / str(profile["golden"])).resolve()
    golden = load_golden(
        golden_path,
        expected_query_schema=str(profile["query_schema"]),
        expected_session_schema=int(profile["session_schema"]),
        expected_source_sha256=load_json(golden_path)["source"]["sha256"],
    )
    identity = _bootstrap_identity(
        profile_path=profile_path,
        inputs_path=inputs_path,
        golden_path=golden_path,
        inputs=inputs,
    )
    fingerprint = compute_run_fingerprint(identity)
    output_root = args.output.resolve()
    active_path = output_root / "active-run.json"
    if args.resume:
        active = load_json(active_path)
        if active.get("run_fingerprint") != fingerprint:
            raise RuntimeError("active run fingerprint does not match current inputs/source")
        run_directory = Path(active["run_directory"])
        if not run_directory.is_dir():
            raise RuntimeError(f"active run directory is missing: {run_directory}")
    else:
        run_directory = create_run_directory(output_root, fingerprint)
        atomic_write_json(
            active_path,
            {
                "run_directory": str(run_directory),
                "run_fingerprint": fingerprint,
                "overall_status": "RUNNING",
            },
        )
    atomic_write_json(run_directory / "environment.json", identity)
    recorder = ComparisonRecorder(run_directory / "comparisons.ndjson")
    commands = CommandExecutor(run_directory)
    commands.outer_job_bytes = int(profile["converter"]["outer_job_bytes"])
    store = PhaseStore(run_directory, fingerprint)
    context = RunContext(
        run_directory=run_directory,
        profile=profile,
        inputs=inputs,
        recorder=recorder,
        commands=commands,
        phase_store=store,
        run_fingerprint=fingerprint,
        golden=golden.raw,
    )
    def validate_identity():
        observed = _bootstrap_identity(profile_path=profile_path, inputs_path=inputs_path,
            golden_path=golden_path, inputs=inputs)
        if observed != identity:
            raise RuntimeError("source/config/repository/session identity changed during this Run")
        p2 = store.load("P2")
        if p2 and p2.get("status") == "PASS":
            evidence = p2["evidence"]
            for name, path in evidence.get("tool_paths", {}).items():
                if sha256_file(Path(path)) != evidence["tool_sha256"][name]:
                    raise RuntimeError(f"locked tool SHA changed: {name}")
    context.validate_identity = validate_identity
    handlers = {
        "P0": run_p0,
        "P1": run_p1,
        "P2": run_p2,
        "P3": run_p3,
        "P4": run_p4,
        "P5": run_p5,
        "P6": run_p6,
        "P7": run_p7,
        "P8": run_p8,
        "P9": _report_phase,
    }
    phases = select_phases(
        tuple(profile["phases"]),
        from_phase=args.from_phase,
        rerun_phase=args.rerun_phase,
    )
    with AcceptanceProgress(run_directory, output_root) as progress:
        context.progress = progress
        commands.progress = progress
        outcomes = AcceptanceRunner(
            context, handlers=handlers, resume=args.resume
        ).run(phases)
        progress.update(status="PHASES_FINISHED", command="report")
    command_line = subprocess.list2cmdline(
        [str(inputs.python_executable), str(Path(__file__).resolve()), *sys.argv[1:], "--resume"]
    )
    result = build_final_result(
        identity=identity,
        outcomes=[outcome.as_dict() for outcome in outcomes],
        comparisons=load_comparison_records(run_directory / "comparisons.ndjson"),
        profile=profile,
        rerun_command=command_line,
        metrics={"commands": [json.loads(line) for line in
            (run_directory / "commands.ndjson").read_text(encoding="utf-8").splitlines()]
            if (run_directory / "commands.ndjson").is_file() else []},
    )
    progress.update(status=result["overall_status"], command="report_complete")
    progress.write_now()
    write_final_reports(run_directory / "report", result)
    atomic_write_json(
        active_path,
        {
            "run_directory": str(run_directory),
            "run_fingerprint": fingerprint,
            "overall_status": result["overall_status"],
        },
    )
    print(
        f"[RUN][END] status={result['overall_status']} report={run_directory / 'report'}",
        flush=True,
    )
    return {"PASSED": 0, "FAILED": 1, "INCOMPLETE": 2}[result["overall_status"]]


if __name__ == "__main__":
    raise SystemExit(main())
