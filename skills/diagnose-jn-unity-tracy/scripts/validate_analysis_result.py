#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Validate AI analysis data and MCP evidence before report rendering."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from report_common import load_json, sha256_file, validate_analysis_and_evidence
from query_native_report import is_query_native, validate_query_native_analysis


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--analysis", required=True, type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--candidates", type=Path)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    analysis = load_json(args.analysis)
    evidence = load_json(args.evidence)
    errors, evidence_by_id, _ = validate_analysis_and_evidence(analysis, evidence, args.evidence.parent)
    if is_query_native(analysis):
        if not args.candidates:
            errors.append("--candidates is required for Query-native workflow 2.0")
        else:
            candidates = load_json(args.candidates)
            errors.extend(validate_query_native_analysis(analysis, candidates, evidence, args.evidence.parent))
            expected_sha = analysis.get("query_scan", {}).get("candidate_manifest_sha256")
            actual_sha = sha256_file(args.candidates)
            if expected_sha != actual_sha:
                errors.append("analysis.query_scan.candidate_manifest_sha256 does not match --candidates")
    result = {
        "schema_version": 1,
        "passed": not errors,
        "error_count": len(errors),
        "errors": errors,
        "signature_count": len(analysis.get("bottleneck_signatures", [])) if isinstance(analysis, dict) else 0,
        "evidence_count": len(evidence_by_id),
    }
    if args.output:
        from report_common import write_json

        write_json(args.output, result)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
