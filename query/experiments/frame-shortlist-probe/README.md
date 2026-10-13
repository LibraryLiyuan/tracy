# Direct EventStore frame shortlist feasibility probe

This isolated experiment proves that frame severity selection can run without
`WorkerTraceSource`, a skeleton snapshot, or CPU/GPU timeline loading. It is **not
the production v4 analyzer**. It does not replace `tracy-analyze.exe`.

It opens an **existing published schema-17 event store** for a `.tracy-stream`.
The existing size/write-time/file-ID validation remains enabled; it does not
rehash the full journal, convert a capture, or build a missing store. Capture
quality (`clean_end`) is preserved separately from complete frame selection.

The native executable reads frame sets and exact frame boundaries, selects the
unique `Player.Frame`, and computes all severity thresholds in one pass. Open
tails and invalid durations are marked and excluded from the denominator.
There is a one-million-total-frame guard because the existing `AllFrames()` API
returns an unbounded vector. Production should use a bounded per-set cursor.

At 60 FPS, strict misses start at 16,666,667 ns. Candidate frames are strictly
greater than 17,000,000 ns, significant frames strictly greater than 20,000,000
ns, and severe frames at least 33,333,334 ns. Severity thresholds use integer
ns; no rounded millisecond JSON is used as input.

## Rebuild on this workspace

Use the matching existing Release store libraries (MSVC x64 `/MD`). Rebuild
those libraries from the same source checkout first if their source or ABI has
changed. The CMake arguments can override the root, libraries and JSON headers.

```powershell
cmake -S E:\tracy\query\experiments\frame-shortlist-probe -B E:\tracy\_build-all\frame-shortlist-probe -G 'Visual Studio 17 2022' -A x64 -DTRACY_JSON_INCLUDE=E:/tracy/_build-all/_cpm/json/b88ca108e0b5a597e859a21658d11fa5f1feb410/include
cmake --build E:\tracy\_build-all\frame-shortlist-probe --config Release --parallel 4
ctest --test-dir E:\tracy\_build-all\frame-shortlist-probe -C Release --output-on-failure
```

Example (choose a **new** output directory; existing ones are rejected):

```powershell
& E:\tracy\_build-all\frame-shortlist-probe\Release\tracy-frame-shortlist-probe.exe F:\JNTracyCapture\Player_20261011_001534\Player_20261011_001534.tracy-stream F:\JNTracyCapture\Player_20261011_001534\analysis-60fps-20261011\v4-feasibility\manual-run-1
python -X utf8 E:\tracy\query\experiments\frame-shortlist-probe\verify_probe.py F:\JNTracyCapture\Player_20261011_001534\analysis-60fps-20261011\v4-feasibility\manual-run-1 F:\JNTracyCapture\Player_20261011_001534\analysis-60fps-20261011\native --latest
```

Python only compares saved native outputs and writes validation receipts. It
does not parse or aggregate CPU/GPU trace events. `summary.json` timing covers
open + frame read + native selection + frame JSON writing, excluding startup,
final summary serialization and the separate Python validation. Memory values
are phase samples of private bytes plus the OS peak working-set counter; they
are not a sampled peak-private guarantee.

Three runs per real capture were measured on 2026-10-11 without flushing the OS
file cache. This is not a cold-store-build benchmark. Results and hashes are in:

`F:\JNTracyCapture\Player_20261011_001534\analysis-60fps-20261011\v4-feasibility`

Planned production interfaces and acceptance gates are documented in
`doc/Tracy-Analyze-v4-Execution-Design-20261011.zh-CN.md`.
