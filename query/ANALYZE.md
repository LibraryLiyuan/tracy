# tracy-analyze v4: native stream candidate scanning

The current classifier ID is `frame-triage-v4.1`. It treats the two exact
`RunTimeDrawStats...BeforeWaitForTargetFPSUpdate` statistics scopes as work,
and `Gfx.WaitForGfxCommandsFromMainThread` as a CPU command-queue wait rather
than a GPU wait. Regression fixtures cover both cases. A fresh 64-representative
run on Player_20261011_001534 retained all 7,033 candidate frame identities and
all 29 severe frames; two near-threshold frames gained the main-work reason.
Standard Query `frame.outliers` still includes an inferred continuous tail;
use `triage.json` complete-frame statistics and do not mix their denominators
or percentile implementations. Native CPU/GPU window data is unchanged.

Requires a `.tracy-stream` with an existing published schema-17 EventStore.
It never converts the full capture into `.tracy`. The fast stage opens the
store directly; full triage/deep queries still reuse the small-domain Worker
skeleton for metadata while reading CPU/GPU timelines from the store.

## Modes

```powershell
# Default: scan all recognized Main/Render/Worker roles, all GPU queues and
# recorded VRAM sources; retain the full independent candidate ledger.
tracy-analyze --trace capture.tracy-stream --output-dir NEW_DIRECTORY --mode triage

# Also inspect up to 24 representative frames and their adjacent frames.
tracy-analyze --trace capture.tracy-stream --output-dir NEW_DIRECTORY --mode diagnose --max-representatives 24

# Fast frame/VRAM shortlist; explicitly does NOT scan CPU/GPU work.
tracy-analyze --trace capture.tracy-stream --output-dir NEW_DIRECTORY --mode shortlist
```

Output must be new or empty. Defaults: `--fps 60`, `--workers 4`,
`--vram-limit-bytes 5368709120` (5 GiB). Both the 5,000,000,000-byte (5 GB)
and 5,368,709,120-byte (5 GiB) reference limits are independently recorded.
The hard limit uses strict `>`; it triggers even when FPS is healthy.
Use `--vram-limit-bytes 5000000000` if the project requirement is 5 GB.
NonLocal system memory, engine physical/logical and DXGI Local usage are not
added together. Primary Segment counter semantics require adapter/UMA context.

## Facts, candidates and scope

- `fast-summary.json`, `fast-frames.ndjson`, `fast-candidates.ndjson`,
  `fast-memory-candidates.ndjson`: bounded store reads with count/cursor checks.
- `frames.ndjson`: exact integer-ns facts plus display ms, independent reasons.
- `candidates.ndjson`: every selected frame, including strict 60 FPS misses,
  simultaneous CPU/GPU signals, waits/unknowns, sustained segments and VRAM.
- `native-candidates.ndjson`: uncropped long CPU self pieces, complete GPU
  root spans (Copy separate), observed memory sample exceedances.
- `triage.json`: counts, reason counts, coverage gaps, representative IDs and
  deferred count. A representative/window budget NEVER caps the candidate file.
- `capture-context.json`: recorded runtime configuration, fetched before heavy
  scanning and retained with cached facts. No automatic controlled-A/B verdict.
- `window-N-cpu.json` / `window-N-gpu.json`: all groups in each selected range;
  numeric path identity includes source and dynamic name, strings expand only
  at output. GPU cursor child reads stay within the actual selected parents.
- `manifest.json`: executable/source identity, effective policy/hash, completed
  phases, output SHA256, native timing and process memory.

Main/Render facts partition the observed hierarchy into nonoverlapping self
intervals. Named work inside waits is work; generic blocking children inherit
the nearest typed wait. Present's unexplained self is Unknown, not GPU blocking.
Multiple recognized role owners are all scanned; role totals can overlap and
must not be treated as a single thread's on-CPU time. CPU facts are instrumented
wall time, and GPU unions/root spans are signals, not confirmed frame critical
paths. Incomplete or invalid scopes and missing domains remain explicit.

Memory detection retains all recorded high samples, including a high-low pair
inside one frame. Frame mapping uses a last-observation hold with a 1 s maximum
age, and reports the bound. The trace cannot establish unsampled peaks. Native
memory candidates survive even outside completed Player.Frame intervals.

`recorded_candidate_enumeration_complete`, `all_domain_coverage_complete`,
`causal_classification_complete`, and `source.capture_clean_end` are separate.
A successfully completed run does not imply that missing timestamps or causal
links were reconstructed. The real fixtures retain timing-quality gaps.

## Cache and recovery

Same-input/same-policy/same-EXE triage stages are stored under
`<capture directory>/.tracy-analyze-cache-v4/<key>`. The key includes recorded
source/data identity, schema, pinned revision, policy and executable SHA. Each
cached file's byte length/SHA is verified before any restoration. `--cache-dir`
changes the directory; `--no-cache` disables automatic reuse/publication.
Cache writes publish only a complete stage via directory rename.

```powershell
tracy-analyze --trace capture.tracy-stream --output-dir NEW_DIRECTORY --resume PREVIOUS_OUTPUT
```

`--resume` reuses a verified complete triage stage into a new output. Policy,
revision, executable or checksum mismatch rejects it. It does not reuse a
session-bound Query cursor or claim page-level cross-process recovery. Failed
or incomplete enrichment requests may be rerun using the same completed stage;
completed deep-window/query output reuse is not automated in this version.
Changing thresholds invalidates this first version's policy-bound stage; fully
policy-independent persisted facts/reselection remain a later optimization.

## Deep query plan and pagination

```json
{
  "windows": [{"start_ns":"1000000000","end_ns":"1100000000","cpu":true,"gpu":true,"export_events":true}],
  "requests": [{"method":"plot.list","params":{"limit":100}}]
}
```

`--plan` accepts up to 64 explicit windows. Without plan windows, `diagnose`
uses short representative windows; default `triage` does no deep windows.
`--skip-triage` can run known windows/requests. `shortlist` rejects deep requests.

Normal `next_cursor` pages are followed within one session, with a receipt per
page (`request-N.json`, `request-N-page-M.json`). Repeated cursor, stale revision,
errors and budget partials stay incomplete; independent requests may continue.
Limits: 1,000 pages and 256 MiB receipts per request. Exit 0 is successful
execution, 2 indicates incomplete enrichment, 1 is an error. Parent/child and
concurrent groups must not be summed. Group distributions use full overlapping
occurrences; frame-clipped inclusive totals are explicitly separate.

SQLite main cache stays at 64 MiB; compact readers use 8 MiB each. Result buffers
and export staging caches are separate from these limits. The full-run private
memory guard is 4 GiB. Fast runs avoid Worker memory.

## Build / verification

```powershell
cmake --build E:\tracy\_build-all\query-dev --config Release --target tracy-analyze tracy-query tracy-frame-triage-tests tracy-event-store-window-scan-tests tracy-analyze-gpu-store-tests --parallel 4
ctest --test-dir E:\tracy\_build-all\query-dev -C Release -R 'tracy-frame-triage|tracy-event-store-window-scan|tracy-query-contract|tracy-query-mcp-transcript|tracy-analyze-gpu-store' --output-on-failure
python -X utf8 E:\tracy\query\tests\verify_analyze_v4.py OUTPUT --baseline FROZEN_V3_OUTPUT --severe 29 --latest
```

Fixtures cover Present retirement, typed waits/pacing/helping, multiple role
owners, DTO/compact equivalence, independent simultaneous signals, memory-only
fast frames, intra-frame/pre-frame peaks, expired observations, and representative
budgets. GPU cursor child fields are checked against direct parent reads.
Validation scripts only inspect saved native outputs, never parse trace events.

Implementation and measurement record:
`E:/tracy/doc/Tracy-Analyze-v4-Implementation-20261011.zh-CN.md`.
