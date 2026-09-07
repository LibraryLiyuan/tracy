"""Map official Query/Audit output to stable N30A metric keys."""

from __future__ import annotations

import re
from typing import Any, Iterable, Mapping

from .comparison_log import ComparisonRecorder
from .comparators import (
    ComparisonResult,
    ComparisonStatus,
    compare_exact,
    parse_exact_int,
)


_CONVERTER_SUMMARY = re.compile(
    r"^generation=(?P<generation>\S+)\s+"
    r"canonical_shards=(?P<canonical_shards>[0-9]+)\s+"
    r"indexed_records=(?P<indexed_records>[0-9]+)\s+"
    r"index_bytes=(?P<index_bytes>[0-9]+)\s+"
    r"gpu_resources=(?P<gpu_resources>[0-9]+)\s+"
    r"gpu_passes=(?P<gpu_passes>[0-9]+)$",
    re.MULTILINE,
)


def _decimal(value: Any) -> str:
    return str(parse_exact_int(value))


def _response_map(responses: Iterable[Mapping[str, Any]]) -> dict[Any, Mapping[str, Any]]:
    result: dict[Any, Mapping[str, Any]] = {}
    for response in responses:
        identifier = response.get("id")
        if identifier in result:
            raise ValueError(f"duplicate evidence response: {identifier!r}")
        data = response.get("data")
        if not isinstance(data, Mapping):
            raise ValueError(f"response {identifier!r} has no data object")
        result[identifier] = data
    return result


def _reason_counts(reason: Any) -> dict[str, str]:
    if not isinstance(reason, str):
        return {}
    result: dict[str, str] = {}
    for component in reason.split(";"):
        key, separator, value = component.partition(":")
        if not separator or not value:
            continue
        try:
            result[key] = _decimal(value)
        except (TypeError, ValueError):
            continue
    return result


def extract_acceptance_metrics(
    responses: Iterable[Mapping[str, Any]],
) -> dict[str, str]:
    by_id = _response_map(responses)
    metrics: dict[str, str] = {}

    counts = by_id.get("counts")
    if isinstance(counts, Mapping):
        for key, value in counts.items():
            metrics[f"trace.counts.{key}"] = _decimal(value)

    validation = by_id.get("gpu-catalog-validation")
    if isinstance(validation, Mapping):
        catalog_counts = validation.get("counts")
        if isinstance(catalog_counts, Mapping):
            catalog_names = {
                "resources": "resources",
                "allocations": "allocations",
                "logical_resources": "logical_resources",
                "parts": "parts",
                "range_records": "ranges",
                "relations": "relations",
                "views": "views",
                "vg_records": "vg_records",
            }
            for source, target in catalog_names.items():
                if source in catalog_counts:
                    metrics[f"gpu.catalog.{target}"] = _decimal(catalog_counts[source])

        verifier = validation.get("independent_verifier")
        if isinstance(verifier, Mapping):
            verifier_fields = (
                "resources",
                "allocations",
                "passes",
                "ranges",
                "direct_member_count",
                "inclusive_member_count",
                "resource_pass_relations",
                "forward_relation_hash",
                "reverse_relation_hash",
                "report_hash",
                "mismatch_count",
                "source_catalog_record_count",
                "source_gpu_catalog_events",
                "source_payload_bytes",
                "source_record_hash",
                "source_reference_relation_count",
            )
            for field in verifier_fields:
                if field in verifier:
                    metrics[f"gpu.verifier.{field}"] = _decimal(verifier[field])

        source_quality = validation.get("source_quality")
        if isinstance(source_quality, Mapping):
            if "gpu_reference_identity_gaps" in source_quality:
                metrics["quality.gpu_reference_identity_gaps"] = _decimal(
                    source_quality["gpu_reference_identity_gaps"]
                )
            if "gpu_resource_identity_gaps" in source_quality:
                metrics["quality.gpu_resource_identity_gaps"] = _decimal(
                    source_quality["gpu_resource_identity_gaps"]
                )
        reason = _reason_counts(validation.get("reason"))
        if "source_cpu_zone_clock_inversion" in reason:
            metrics["quality.invalid_cpu_zone_timings"] = reason[
                "source_cpu_zone_clock_inversion"
            ]
        if "source_scheduling_gap" in reason:
            metrics["quality.scheduling_source_gaps"] = reason[
                "source_scheduling_gap"
            ]
        if "source_gpu_resource_identity_gap" in reason:
            metrics.setdefault(
                "quality.gpu_resource_identity_gaps",
                reason["source_gpu_resource_identity_gap"],
            )
        unresolved = validation.get("unresolved_breakdown")
        if isinstance(unresolved, Mapping) and "converter_created" in unresolved:
            metrics["quality.converter_unresolved"] = _decimal(
                unresolved["converter_created"]
            )
        transport = validation.get("transport")
        if isinstance(transport, Mapping) and "checksum_failures" in transport:
            metrics["quality.committed_checksum_failures"] = _decimal(
                transport["checksum_failures"]
            )

    peak = by_id.get("gpu-memory-peak")
    if isinstance(peak, Mapping):
        if "engine_known_physical_peak_bytes" in peak:
            metrics["gpu.memory.engine_known_physical_peak_bytes"] = _decimal(
                peak["engine_known_physical_peak_bytes"]
            )
        if "peak_time_ns" in peak:
            metrics["gpu.memory.engine_known_physical_peak_time_ns"] = _decimal(
                peak["peak_time_ns"]
            )

    coverage = by_id.get("capture-coverage")
    if isinstance(coverage, Mapping):
        producers = coverage.get("producers")
        if isinstance(producers, list):
            for producer in producers:
                if not isinstance(producer, Mapping) or producer.get("key") != "gpu.catalog":
                    continue
                counters = producer.get("counters")
                if not isinstance(counters, Mapping):
                    continue
                if "dropped" in counters:
                    metrics["quality.gpu_catalog_dropped"] = _decimal(
                        counters["dropped"]
                    )
                if "overflow" in counters:
                    metrics["quality.gpu_catalog_overflow"] = _decimal(
                        counters["overflow"]
                    )
                break
    return metrics


def parse_converter_summary(output: str) -> dict[str, str]:
    matches = list(_CONVERTER_SUMMARY.finditer(output))
    if len(matches) != 1:
        raise ValueError(
            f"expected exactly one converter completion summary, found {len(matches)}"
        )
    values = matches[0].groupdict()
    return {
        "session.canonical_shards": _decimal(values["canonical_shards"]),
        "session.indexed_records": _decimal(values["indexed_records"]),
        "session.index_bytes": _decimal(values["index_bytes"]),
        "session.gpu_resources": _decimal(values["gpu_resources"]),
        "session.gpu_passes": _decimal(values["gpu_passes"]),
    }


def compare_metric_maps(
    expected: Mapping[str, Any],
    actual: Mapping[str, Any],
    recorder: ComparisonRecorder,
) -> list[ComparisonResult]:
    results: list[ComparisonResult] = []
    for key in sorted(expected):
        expected_value = parse_exact_int(expected[key])
        if key not in actual:
            result = ComparisonResult(
                name=key,
                status=ComparisonStatus.FAIL,
                expected=expected_value,
                actual="<missing>",
                delta=None,
                rule="exact_integer",
                detail="required metric is absent",
            )
        else:
            try:
                result = compare_exact(key, expected_value, actual[key])
            except (TypeError, ValueError) as error:
                result = ComparisonResult(
                    name=key,
                    status=ComparisonStatus.FAIL,
                    expected=expected_value,
                    actual=actual[key],
                    delta=None,
                    rule="exact_integer",
                    detail=f"actual value is not an exact integer: {error}",
                )
        recorder.record(result)
        results.append(result)
    return results
