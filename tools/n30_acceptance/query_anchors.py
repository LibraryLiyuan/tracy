"""Read-only deterministic Session anchors, compared through public Query/MCP.

The native helper reads immutable indexes, not Query JSON. Python performs every
comparison, preserves exact integers, and prints each expected/actual/delta.
"""

from __future__ import annotations

import json
import hashlib
from pathlib import Path
from typing import Any, Mapping

from .comparators import compare_equal, compare_exact, compare_true, parse_exact_int
from .io_utils import atomic_write_json


def require_anchors(value: Mapping[str, Any], generation: str) -> Mapping[str, Any]:
    if value.get("schema") != 1 or value.get("generation") != generation:
        raise ValueError("anchor schema or pinned generation mismatch")
    for field in ("shards", "frames", "resources", "images"):
        if not isinstance(value.get(field), list) or not value[field]:
            raise ValueError(f"anchor output omitted nonempty {field}")
    for field in ("peak", "largest_allocation", "max_fanout_pass"):
        if not isinstance(value.get(field), Mapping):
            raise ValueError(f"anchor output omitted {field}")
    return value


def compare_fields(name: str, expected: Mapping[str, Any], actual: Mapping[str, Any]):
    results = []
    for key, value in expected.items():
        label = f"{name}.{key}"
        if key not in actual:
            results.append(compare_equal(label, value, "<missing>"))
        elif isinstance(value, int) and not isinstance(value, bool):
            try:
                results.append(compare_exact(label, value, actual[key]))
            except (TypeError, ValueError):
                results.append(compare_equal(label, value, actual[key]))
        else:
            results.append(compare_equal(label, value, actual[key]))
    return results


def relation_membership(rows, field: str, identity: int) -> bool:
    return any(isinstance(row, Mapping) and field in row and row[field] is not None
               and parse_exact_int(row[field]) == identity for row in rows)


def resolve_gpu_frame_alias(records, gpu_frame_index: int) -> int:
    identities = {parse_exact_int(row["frame_id"]) for row in records
                  if parse_exact_int(row["domain_id"]) == 4
                  and parse_exact_int(row["domain_index"]) == gpu_frame_index}
    if len(identities) != 1:
        raise ValueError("GPU frame alias unavailable or ambiguous; numeric/time inference forbidden")
    return identities.pop()


def run_anchor_matrix(context, directory: Path):
    # Imported here so phase_p6 can use this module without a cyclic import.
    from .phase_p6 import (
        _new_mcp_client, _initialize_mcp, open_mcp_session, _inspect,
        _close_trace, require_tool_data, _outcome,
    )
    session = context.inputs.fresh_session_30m
    generation = (session / "CURRENT").read_text(encoding="utf-8").strip()
    helper = context.inputs.build_query / "Release" / "tracy-trace-session-scale-corpus.exe"
    results = []

    def record(result):
        results.append(context.recorder.record(result))

    anchors = None
    directory.mkdir(parents=True, exist_ok=False)
    for ordinal in (1, 2):
        result, stdout, _ = context.commands.run_text(
            f"P6-anchor-selection-{ordinal}",
            [str(helper), "inspect", "--mode", "anchors", "--source",
             str(context.inputs.source_30m), "--session", str(session)],
            cwd=context.inputs.worktree, timeout_seconds=600,
            no_progress_timeout_seconds=120, echo_output=False,
        )
        record(compare_exact(f"P6.anchors.selection_{ordinal}.exit", 0, result.exit_code))
        if result.exit_code != 0:
            raise ValueError("native read-only anchor selection failed; see raw command stderr")
        value = require_anchors(json.loads(stdout), generation)
        atomic_write_json(directory / f"anchors-selection-{ordinal}.json", value)
        if anchors is not None:
            digest = lambda document: hashlib.sha256(json.dumps(document, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
            record(compare_equal("P6.anchors.deterministic_regeneration.sha256", digest(anchors), digest(value)))
        anchors = value
    atomic_write_json(directory / "anchors.json", anchors)
    client = _new_mcp_client(
        context.inputs.build_query / "Release" / "tracy-query.exe",
        context.inputs.scratch_root.parent, directory / "mcp",
    )
    observations = []
    with client:
        _initialize_mcp(client)
        trace_id, _ = open_mcp_session(client, session, timeout_seconds=120)

        def query(method, params):
            structured = _inspect(client, trace_id, method, params, timeout_seconds=60)
            data = require_tool_data(structured)
            observations.append({"method": method, "params": params,
                                 "warnings": structured.get("warnings"),
                                 "budget": structured.get("budget")})
            budget = structured.get("budget", {})
            if structured.get("partial") is True or (isinstance(budget, Mapping) and budget.get("partial") is True):
                raise ValueError(f"anchor Query {method} returned budget-partial data")
            return structured, data

        def fields(label, expected, actual):
            for result in compare_fields(f"P6.anchors.{label}", expected, actual):
                record(result)

        def all_rows(method, params, collection, *, maximum_pages=256):
            cursor = None
            seen = set()
            for _ in range(maximum_pages):
                requested = {**params, "limit": 1000}
                if cursor is not None:
                    requested["cursor"] = cursor
                response, data = query(method, requested)
                rows = data.get(collection)
                if not isinstance(rows, list):
                    raise ValueError(f"{method} omitted {collection}")
                yield from rows
                cursor = response.get("page", {}).get("next_cursor")
                if not cursor:
                    return
                if cursor in seen:
                    raise ValueError(f"{method} returned repeated cursor")
                seen.add(cursor)
            raise ValueError(f"{method} exceeded bounded anchor pagination; coverage incomplete")

        _, peak = query("gpu.memory.peak", {})
        fields("peak", anchors["peak"], peak)
        _, quality = query("gpu.catalog.validation", {})
        atomic_write_json(directory / "anchor-quality.json", quality)

        for index, frame in enumerate(anchors["frames"]):
            _, actual = query("frame.get", {"frame_set": frame["frame_set_index"], "index": frame["index"]})
            fields(f"frame_{index}", {key: frame[key] for key in ("index", "begin_ns", "end_ns", "complete")}, actual)
        # Representative time windows are bounded by frames, not entire shards.
        # Time overlap is used only to select rows; it is never called causality.
        sampled_shards = set()
        for frame in anchors["frames"]:
            if frame["shard_id"] in sampled_shards or frame["end_ns"] is None:
                continue
            sampled_shards.add(frame["shard_id"])
            bounds = {"start_ns": str(frame["begin_ns"]), "end_ns": str(frame["end_ns"]), "limit": 3}
            for method, collection in (("zone.cpu.search", "zones"), ("zone.gpu.search", "zones"),
                                       ("sample.list", "samples"), ("context_switch.range", "context_switches")):
                _, data = query(method, bounds)
                expected_window = next(window for window in anchors["windows"] if window["shard_id"] == frame["shard_id"])
                record(compare_equal(f"P6.anchors.shard_{frame['shard_id']}.{method}.ordered_refs",
                    expected_window[method], [row["ref"] for row in data.get(collection, [])]))
                # Store the domain shape in addition to the complete MCP transcript.
                atomic_write_json(directory / f"shard-{frame['shard_id']}-{method}.json", data)
                if method == "zone.cpu.search":
                    for zone in data.get(collection, [])[:1]:
                        _, detail = query("zone.cpu.get", {"ref": zone["ref"]})
                        fields(f"frame_cpu_{frame['shard_id']}", {key: zone[key] for key in
                            ("ref", "source_location_ref", "start_ns", "end_ns", "callstack_ref") if key in zone}, detail)
                        if detail.get("callstack_ref"):
                            query("callstack.frames", {"callstack": detail["callstack_ref"]})
                        if detail.get("source_location_ref"):
                            _, locations = query("source.locations", {"ref": detail["source_location_ref"], "limit": 1})
                            record(compare_true(f"P6.anchors.frame_cpu_{frame['shard_id']}.source_link",
                                any(value.get("ref") == detail["source_location_ref"] for value in locations.get("source_locations", []))))
                        if detail.get("callsite_id"):
                            query("source.callsite", {"callsite_id": detail["callsite_id"]})

        resources = list(anchors["resources"])
        if anchors.get("cross_shard_resource"):
            resources.append(anchors["cross_shard_resource"])
        for expected in resources:
            resource_id = expected["resource_id"]
            _, actual = query("gpu.resource.get", {"resource_id": str(resource_id)})
            fields(f"resource_{resource_id}", expected, actual)
            _, lifetime = query("gpu.resource.lifetime", {"resource_id": str(resource_id)})
            fields(f"resource_{resource_id}.lifetime", expected, lifetime)
            _, allocation = query("gpu.resource.allocations", {"resource_id": str(resource_id)})
            if expected["allocation_id"] is not None:
                record(compare_true(f"P6.anchors.resource_{resource_id}.allocation_link",
                    relation_membership(allocation.get("allocations", []), "allocation_id", expected["allocation_id"])))

        largest = anchors["largest_allocation"]
        record(compare_true("P6.anchors.largest_allocation.queryable_resource_link", largest["resource_id"] is not None))
        if largest["resource_id"] is not None:
            _, actual = query("gpu.resource.allocations", {"resource_id": str(largest["resource_id"])})
            rows = actual.get("allocations", [])
            match = next((row for row in rows if parse_exact_int(row["allocation_id"]) == largest["allocation_id"]), {})
            fields("largest_allocation", {key: largest[key] for key in ("allocation_id", "size_bytes")}, match)

        expected_pass = anchors["max_fanout_pass"]
        pass_id = expected_pass["pass_id"]
        _, actual = query("gpu.pass.resources", {"pass_id": str(pass_id), "limit": 3})
        fields("max_fanout_pass", {key: expected_pass[key] for key in
               ("pass_id", "resource_count", "referenced_physical_bytes", "complete", "truncated")}, actual)
        frame_passes = all_rows("gpu.pass.by_frame", {"frame_id": str(expected_pass["frame_id"])}, "passes")
        record(compare_true("P6.anchors.frame_to_pass", any(parse_exact_int(row["pass_id"]) == pass_id for row in frame_passes)))
        for resource_id in expected_pass["resource_ids"]:
            reverse = all_rows("gpu.resource.references", {"resource_id": str(resource_id)}, "passes")
            record(compare_true(f"P6.anchors.pass_{pass_id}.resource_{resource_id}.reverse_link",
                any(parse_exact_int(row["pass_id"]) == pass_id and row.get("direct") is True for row in reverse)))
        correlated_id = resolve_gpu_frame_alias(expected_pass["frame_identity_aliases"], expected_pass["frame_id"])
        _, identity = query("frame.identity", {"frame_id": str(correlated_id)})
        fields("gpu_pass.frame_identity", {"frame_id": correlated_id}, identity.get("identity", {}))
        record(compare_exact("P6.anchors.gpu_frame_alias.roundtrip", correlated_id,
            resolve_gpu_frame_alias(identity["identity"]["events"], expected_pass["frame_id"])))
        _, jobs = query("timeline.correlated_slice", {"ref": identity["identity"]["ref"], "max_nodes": 10000})
        for job in jobs.get("jobs", [])[:3]:
            fields(f"job_{job['job_id']}.frame", {"origin_frame_ref": identity["identity"]["ref"]}, job)
            _, detail = query("job.get", {"ref": job["ref"]})
            fields(f"job_{job['job_id']}", {key: job[key] for key in
                ("job_id", "origin_frame_ref", "stage_count", "completed_ns", "wait") if key in job}, detail)
            record(compare_exact(f"P6.anchors.job_{job['job_id']}.stages", detail["stage_count"], len(detail.get("stages", []))))
            query("job.dependencies", {"ref": job["ref"], "limit": 3})
        for image in anchors["images"]:
            _, metadata = query("frame_image.metadata", {"ref": image["ref"]})
            fields(f"image_{image['raw_frame_index']}", image, metadata)
            if image["frame_ref"]:
                _, frame = query("frame.get", {"ref": image["frame_ref"]})
                fields(f"image_{image['raw_frame_index']}.frame", {"ref": image["frame_ref"]}, frame)
            query("frame_image.raw", {"ref": image["ref"], "max_bytes": 64})
        _close_trace(client, trace_id)
    atomic_write_json(directory / "anchor-query-observations.json", observations)
    return _outcome(results, success="deterministic anchors and public Query chains compared")
