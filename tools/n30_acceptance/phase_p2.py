"""P2 fresh toolchain build and native test gate."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
from typing import Any, Mapping

from .acceptance_runner import PhaseOutcome
from .comparison_log import ComparisonRecorder
from .comparators import (
    ComparisonStatus,
    compare_at_least,
    compare_equal,
    compare_exact,
    compare_true,
)
from .io_utils import sha256_file
from .status import AcceptanceStatus


@dataclass(frozen=True)
class ToolCommand:
    label: str
    command: tuple[str, ...]
    cwd: Path
    timeout_seconds: float


@dataclass(frozen=True)
class ToolchainObservation:
    command_exit_codes: Mapping[str, int]
    required_tools: Mapping[str, bool]
    ctest: Mapping[str, int]
    query_schema: str
    tool_sha256: Mapping[str, str]
    tool_paths: Mapping[str, str] | None = None


_CTEST = re.compile(
    r"(?P<percent>[0-9]+)% tests passed, "
    r"(?P<failed>[0-9]+) tests failed out of (?P<total>[0-9]+)"
)
_QUERY_SCHEMA = re.compile(r"\bschema\s+(?P<schema>[0-9]+\.[0-9]+\.[0-9]+)\b")


def parse_ctest_summary(output: str) -> dict[str, int]:
    matches = list(_CTEST.finditer(output))
    if len(matches) != 1:
        raise ValueError(f"expected exactly one CTest summary, found {len(matches)}")
    return {
        "passed_percent": int(matches[0].group("percent"), 10),
        "failed": int(matches[0].group("failed"), 10),
        "total": int(matches[0].group("total"), 10),
    }


def parse_query_schema(output: str) -> str:
    matches = list(_QUERY_SCHEMA.finditer(output))
    if len(matches) != 1:
        raise ValueError(f"expected exactly one Query schema, found {len(matches)}")
    return matches[0].group("schema")


def build_toolchain_commands(
    *,
    cmake: Path,
    ctest: Path,
    worktree: Path,
    capture_build: Path,
    query_build: Path,
    dependency_cache: Path,
    parallel: int = 8,
) -> tuple[ToolCommand, ...]:
    generator = "Visual Studio 18 2026"
    common = (
        "-G",
        generator,
        "-A",
        "x64",
        "-DBUILD_TESTING=ON",
        "-DNO_STATISTICS=OFF",
        f"-DCPM_SOURCE_CACHE={dependency_cache.as_posix()}",
    )
    capture_targets = (
        "tracy-stream-convert",
        "tracy-stream-convert-legacy",
        "tracy-trace-compare",
        "tracy-session-inventory",
    )
    query_targets = (
        "tracy-query",
        "tracy-gpu-analysis-build",
        "tracy-query-contract-tests",
        "tracy-gpu-analysis-tests",
        "tracy-trace-session-store-tests",
        "tracy-trace-session-inventory-tests",
        "tracy-trace-session-scale-corpus",
        "tracy-query-mcp-transcript-tests",
        "tracy-gpu-analysis-corpus",
    )
    return (
        ToolCommand(
            "P2-capture-configure",
            (
                str(cmake), "-S", str(worktree / "capture"), "-B", str(capture_build),
                *common, "-DJN_STREAM_CONVERT_DEV_TOOLS=ON",
            ),
            worktree,
            900,
        ),
        ToolCommand(
            "P2-capture-build",
            (
                str(cmake), "--build", str(capture_build), "--config", "Release",
                "--target", *capture_targets, "--parallel", str(parallel),
            ),
            worktree,
            7200,
        ),
        ToolCommand(
            "P2-query-configure",
            (
                str(cmake), "-S", str(worktree / "query"), "-B", str(query_build),
                *common,
            ),
            worktree,
            900,
        ),
        ToolCommand(
            "P2-query-build",
            (
                str(cmake), "--build", str(query_build), "--config", "Release",
                "--target", *query_targets, "--parallel", str(parallel),
            ),
            worktree,
            10800,
        ),
        ToolCommand(
            "P2-query-ctest",
            (
                str(ctest), "--test-dir", str(query_build), "-C", "Release",
                "--output-on-failure", "-j", str(max(1, parallel // 2)),
            ),
            worktree,
            10800,
        ),
    )


def evaluate_toolchain(
    observation: ToolchainObservation,
    *,
    expected_query_schema: str,
    minimum_ctest_count: int,
    recorder: ComparisonRecorder,
) -> PhaseOutcome:
    checks = []
    for label, exit_code in sorted(observation.command_exit_codes.items()):
        checks.append(compare_exact(f"P2.command.{label}.exit_code", 0, exit_code))
    for label, exists in sorted(observation.required_tools.items()):
        checks.append(compare_true(f"P2.tool.{label}.exists", exists))
    checks.extend(
        (
            compare_exact("P2.ctest.passed_percent", 100, observation.ctest.get("passed_percent", 0)),
            compare_exact("P2.ctest.failed", 0, observation.ctest.get("failed", -1)),
            compare_at_least("P2.ctest.total", minimum_ctest_count, observation.ctest.get("total", 0)),
            compare_equal("P2.query.schema", expected_query_schema, observation.query_schema),
        )
    )
    for check in checks:
        recorder.record(check)
    failures = [check.name for check in checks if check.status == ComparisonStatus.FAIL]
    if failures:
        return PhaseOutcome(
            "P2",
            AcceptanceStatus.HARNESS_ERROR,
            f"toolchain build/test gate failed ({len(failures)} comparisons)",
            repair_direction="inspect the isolated configure/build/CTest logs before any Trace conversion",
            evidence={
                "failed_comparisons": failures,
                "tool_sha256": dict(observation.tool_sha256),
            },
        )
    return PhaseOutcome(
        "P2",
        AcceptanceStatus.PASS,
        f"toolchain built and {observation.ctest['total']} native tests passed",
        evidence={"tool_sha256": dict(observation.tool_sha256), "tool_paths": dict(observation.tool_paths or {})},
    )


def run_p2(context) -> PhaseOutcome:
    policy = context.profile.get("toolchain", {})
    parallel = int(policy.get("parallel", 8))
    plans = build_toolchain_commands(
        cmake=context.inputs.cmake_executable,
        ctest=context.inputs.ctest_executable,
        worktree=context.inputs.worktree,
        capture_build=context.inputs.build_capture,
        query_build=context.inputs.build_query,
        dependency_cache=context.inputs.dependency_cache,
        parallel=parallel,
    )
    exit_codes: dict[str, int] = {}
    command_output: dict[str, str] = {}
    for plan in plans:
        result, stdout, stderr = context.commands.run_text(
            plan.label,
            plan.command,
            cwd=plan.cwd,
            timeout_seconds=plan.timeout_seconds,
            no_progress_timeout_seconds=600,
        )
        exit_codes[plan.label] = result.exit_code
        command_output[plan.label] = stdout + "\n" + stderr
        if result.exit_code != 0:
            break

    ctest = {"passed_percent": 0, "failed": -1, "total": 0}
    if "P2-query-ctest" in command_output:
        try:
            ctest = parse_ctest_summary(command_output["P2-query-ctest"])
        except ValueError:
            pass

    capture_release = context.inputs.build_capture / "Release"
    query_release = context.inputs.build_query / "Release"
    tools = {
        "converter": capture_release / "tracy-stream-convert.exe",
        "legacy_converter": capture_release / "tracy-stream-convert-legacy.exe",
        "trace_compare": capture_release / "tracy-trace-compare.exe",
        "session_inventory": capture_release / "tracy-session-inventory.exe",
        "query": query_release / "tracy-query.exe",
        "gpu_analysis_builder": query_release / "tracy-gpu-analysis-build.exe",
        "gpu_analysis_corpus": query_release / "tracy-gpu-analysis-corpus.exe",
        "trace_session_scale_corpus": query_release / "tracy-trace-session-scale-corpus.exe",
    }
    required_tools = {name: path.is_file() for name, path in tools.items()}
    tool_sha256 = {
        name: sha256_file(path) for name, path in tools.items() if path.is_file()
    }
    query_schema = "unavailable"
    query_path = tools["query"]
    if query_path.is_file():
        result, stdout, stderr = context.commands.run_text(
            "P2-query-version", (str(query_path), "--version"), cwd=query_release
        )
        exit_codes["P2-query-version"] = result.exit_code
        if result.exit_code == 0:
            try:
                query_schema = parse_query_schema(stdout + "\n" + stderr)
            except ValueError:
                pass
    observation = ToolchainObservation(
        command_exit_codes=exit_codes,
        required_tools=required_tools,
        ctest=ctest,
        query_schema=query_schema,
        tool_sha256=tool_sha256,
        tool_paths={name: str(path) for name, path in tools.items()},
    )
    return evaluate_toolchain(
        observation,
        expected_query_schema=str(context.profile["query_schema"]),
        minimum_ctest_count=int(policy.get("minimum_ctest_count", 7)),
        recorder=context.recorder,
    )
