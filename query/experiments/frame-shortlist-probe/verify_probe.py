"""Compare native probe output with frozen native v3 frame facts; no trace parsing.

Usage: python -X utf8 verify_probe.py PROBE_DIR V3_RESULT_DIR [--latest]
Writes a validation receipt next to the probe's results.
"""
import argparse
import hashlib
import json
from pathlib import Path


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def rows(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8-sig").splitlines() if line]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe_dir", type=Path)
    parser.add_argument("v3_dir", type=Path)
    parser.add_argument("--latest", action="store_true", help="Also check known 20261011 regression fixtures")
    args = parser.parse_args()
    output, baseline = args.probe_dir, args.v3_dir
    fresh = rows(output / "frames.ndjson")
    expected = rows(baseline / "frames.ndjson")
    summary = read(output / "summary.json")
    triage = read(baseline / "triage.json")
    actual = [r for r in fresh if r["quality"] == "complete"]
    assert len(actual) == len(expected) == summary["complete_frames"] == triage["frame_set"]["complete_frames"]
    assert len({r["index"] for r in actual}) == len(actual), "Duplicate frame index"
    for new, old in zip(actual, expected):
        for key in ("index", "start_ns", "end_ns"):
            assert new[key] == old[key], (key, new, old)
        # Validation of serialized exact duration, not generation of analysis facts.
        assert int(new["duration_ns"]) == int(old["end_ns"]) - int(old["start_ns"])
    assert summary["strict_gt_budget"] == triage["frame_time"]["over_budget_frames"]
    assert len(summary["severe_frames"]) == summary["severe_ge_2_budget_count"]
    assert [r for r in actual if r["severe_ge_2_budget"]] == summary["severe_frames"]
    assert summary["open_frames"] == triage["frame_set"]["excluded_continuous_tail"]
    if args.latest:
        assert summary["source_sha256"] == "cc50bd5009353b14097e204a8f2330dfcd9524e8897f7eb6307e20b06937721e"
        for name, folder in (("candidate_gt_17ms", "threshold-17ms"), ("significant_gt_20ms", "threshold-20ms"), ("severe_ge_2_budget_count", "threshold-33ms")):
            reference = read(baseline.parent / folder / "triage.json")
            assert summary[name] == reference["frame_time"]["over_budget_frames"]
        selected = {r["index"]: r for r in summary["severe_frames"]}
        assert selected[8534]["duration_ns"] == "60247072"
        assert selected[9272]["duration_ns"] == "60487731"
    receipt = {
        "validation": "passed",
        "exact_boundary_and_index_comparisons": len(actual),
        "baseline_dir": str(baseline.resolve()),
        "probe_dir": str(output.resolve()),
        "source_sha256": summary["source_sha256"],
        "regression_fixtures_20261011": args.latest,
        "files": [{"path": str(p.resolve()), "sha256": sha(p)} for p in
                  (output / "frames.ndjson", output / "summary.json", baseline / "frames.ndjson", baseline / "triage.json")],
    }
    (output / "validation.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"validation": "passed", "frames_compared": len(actual), "output": str(output)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
