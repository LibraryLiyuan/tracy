"""Validate saved native v4 results against frozen native facts (no trace IO).

python -X utf8 verify_analyze_v4.py OUTPUT --baseline V3_DIR --severe 29 --latest
Optional --gpu-equivalent PREVIOUS_V4_DIR compares exact GPU group outputs.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def rows(path):
    with path.open(encoding="utf-8-sig") as source:
        for line in source:
            if line.strip():
                yield json.loads(line)


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--severe", type=int, required=True)
    parser.add_argument("--latest", action="store_true")
    parser.add_argument("--gpu-equivalent", type=Path)
    args = parser.parse_args()
    root = args.output
    manifest = read(root / "manifest.json")
    assert manifest["complete"] and manifest["cpu_gpu_scanned"]
    assert manifest["recorded_candidate_enumeration_complete"]
    for f in manifest["files"]:
        assert Path(f["path"]).name == f["path"]
        p = root / f["path"]
        assert p.stat().st_size == f["bytes"] and sha(p) == f["sha256"], str(p)
    baseline = list(rows(args.baseline / "frames.ndjson"))
    actual = list(rows(root / "frames.ndjson"))
    assert len(actual) == len(baseline)
    expected = {}
    reasons = Counter()
    for new, old in zip(actual, baseline):
        for key in ("index", "start_ns", "end_ns"):
            assert new[key] == old[key], (key, new["index"])
        duration = int(new["duration_ns"])
        assert duration == int(old["end_ns"]) - int(old["start_ns"])
        assert 0 <= int(new["gpu_union_ns"]) <= duration
        assert 0 <= int(new["gpu_copy_union_ns"]) <= duration
        main_keys = ("main_active_ns", "main_unknown_ns", "main_wait_present_ns", "main_wait_job_ns", "main_wait_other_ns", "main_pacing_ns")
        render_keys = ("render_active_ns", "render_unknown_ns", "render_wait_gpu_ns", "render_wait_other_ns")
        # These frozen captures each have one Main and one Render owner.
        for fields in (main_keys, render_keys):
            values = [int(new[k]) for k in fields]
            assert all(v >= 0 for v in values) and sum(values) <= duration, (new["index"], fields, values)
        assert new["selected"] == bool(new["reasons"])
        if new["selected"]:
            expected[new["candidate_id"]] = new["reasons"]
            reasons.update(new["reasons"])
    selected = list(rows(root / "candidates.ndjson"))
    assert len(selected) == len(expected)
    assert {r["candidate_id"]: r["reasons"] for r in selected} == expected
    triage = read(root / "triage.json")
    assert dict(reasons) == triage["selection"]["reason_counts"]
    assert triage["selection"]["severe_frames"] == args.severe
    assert sum(bool(r["severe"]) for r in selected) == args.severe
    native = list(rows(root / "native-candidates.ndjson"))
    assert len({r["candidate_id"] for r in native}) == len(native) == triage["native_candidates_written"]
    if args.latest:
        lookup = {r["index"]: r for r in selected}
        for index in (8534, 9272, 10047):
            assert index in lookup
        assert int(lookup[8534]["render_active_ns"]) >= 34894992
        assert int(lookup[9272]["render_active_ns"]) >= 38502401
        assert int(lookup[8534]["render_wait_gpu_ns"]) < 1000000
        assert int(lookup[9272]["render_wait_gpu_ns"]) < 1000000
        assert {"main_work_budget", "render_work_budget", "gpu_window_budget"} <= set(lookup[10047]["reasons"])
        representatives = {r["index"] for r in triage["selection"]["representatives"]}
        assert {8534, 9272, 10047} <= representatives
    pages_verified = {}
    for request in manifest.get("requests", []):
        index = request["index"]
        first = read(root / f"request-{index}.json")
        method = first["request"]["method"]
        ids, page = [], 0
        response = first["response"]
        while True:
            assert response["ok"] and not response.get("partial", False)
            payload = response["data"]
            key = "plots" if method == "plot.list" else "children" if method == "zone.cpu.tree" else None
            if key:
                ids.extend(item["ref"] for item in payload[key])
            cursor = response.get("page", {}).get("next_cursor")
            if cursor is None:
                break
            page += 1
            receipt = read(root / f"request-{index}-page-{page}.json")
            assert receipt["request"]["params"]["cursor"] == cursor
            response = receipt["response"]
        assert len(ids) == len(set(ids))
        if method == "zone.cpu.tree":
            assert len(ids) == first["response"]["data"]["root"]["child_count"]
        if method == "plot.list" and args.latest:
            assert len(ids) == 438
        pages_verified[str(index)] = {"method": method, "pages": page + 1, "unique_items": len(ids)}
    equivalent = []
    if args.gpu_equivalent:
        for p in sorted(args.gpu_equivalent.glob("window-*-gpu.json")):
            old, new = read(p), read(root / p.name)
            old.pop("elapsed_ms"); new.pop("elapsed_ms")
            assert old == new, p.name
            equivalent.append(p.name)
    validation = {
        "validation": "passed", "frames_compared": len(actual),
        "candidate_ids_and_reason_sets_verified": len(selected),
        "severe_candidates_verified": args.severe,
        "native_candidate_ids_verified": len(native), "pagination": pages_verified,
        "exact_gpu_outputs_verified": equivalent,
        "manifest_sha256": sha(root / "manifest.json"),
        "baseline_frames_sha256": sha(args.baseline / "frames.ndjson"),
        "scope": "Validation of saved native outputs; no trace parsing or new performance aggregation",
    }
    (root / "validation.json").write_text(json.dumps(validation, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(validation, ensure_ascii=False))


if __name__ == "__main__":
    main()
