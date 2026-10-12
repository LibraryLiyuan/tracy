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

from report_common import ValidationFailure, load_json, sha256_file, validate_analysis_and_evidence, validation_result
from query_native_report import is_query_native, validate_query_native_analysis
from candidate_manifest import load_candidate_manifest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--analysis", required=True, type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--candidates", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--legacy-regression", action="store_true", help="Historical fixtures only; never marks analysis complete")
    return parser.parse_args()


def validate() -> int:
    args = parse_args()
    analysis = load_json(args.analysis)
    evidence = load_json(args.evidence)
    errors, evidence_by_id, _ = validate_analysis_and_evidence(analysis, evidence, args.evidence.parent)
    if is_query_native(analysis) or args.candidates:
        if not args.candidates:
            errors.append("--candidates is required for Query-native workflow 2.0")
        else:
            candidates = load_candidate_manifest(args.candidates)
            errors.extend(validate_query_native_analysis(analysis, candidates, evidence, args.evidence.parent))
            expected_sha = analysis.get("query_scan", {}).get("candidate_manifest_sha256")
            actual_sha = sha256_file(args.candidates)
            if expected_sha != actual_sha:
                errors.append("analysis.query_scan.candidate_manifest_sha256 does not match --candidates")
    model = {}
    if (is_query_native(analysis) or args.candidates) and not args.legacy_regression and args.candidates:
        from performance_review import validate_review
        new_errors, model = validate_review(analysis, candidates, evidence, args.evidence.parent)
        errors.extend(new_errors)
    result = {
        **validation_result(analysis, errors),
        "signature_count": len(analysis.get("bottleneck_signatures", [])) if isinstance(analysis, dict) else 0,
        "evidence_count": len(evidence_by_id),
    }
    if not args.legacy_regression and (is_query_native(analysis) or args.candidates):
        from performance_report import status
        result.update(status(analysis, model, errors))
    if args.legacy_regression:
        result.update(analysis_complete=False, legacy_regression=True)
    if args.output:
        from report_common import write_json

        write_json(args.output, result)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    return 0


def main() -> int:
    try:
        return validate()
    except ValidationFailure as exc:
        for error in exc.errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
