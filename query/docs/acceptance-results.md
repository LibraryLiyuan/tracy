# v1 local acceptance results

This file records the Windows x64 Release acceptance run performed on 2026-07-22–23. The trace root was `C:\CodeProjects\GodotProjects`; large captures remain local and are not committed.

## Reproducible baseline

- Base branch/head before v1 work: `feature-StructuredData` at `a7813c1105f037745dd6c9d9f5f1b48025bca9e7`, reached by the planned ff-only merge of `feature-GPUMemory`.
- Included GPU Memory commits: `90385d6f`, `7b473b33`, and `a7813c11`.
- Generator/toolchain: Visual Studio 18 2026 x64, MSVC 19.51.36248.0, MSBuild 18.7.8, and CMake 4.3.1-msvc1.
- Dependency downloads were resolved from `C:\CodeProjects\GodotProjects\tracy-profiler-frame-memory-build\.cpm-cache`; that older build tree was not used for binary acceptance.
- Formal Query and Profiler builds use `-DGIT_REV=HEAD`; the original worktree and packaged Profiler are kept untouched until the final fast-forward.
- The formal `a7813c11` baseline Profiler was 26,539,008 bytes with SHA-256 `BADB83C4392FBB595258F9EF873C3528E69DBB1E7E1D7EC36C39766F5534B3C2`.

## Automated and build gates

- Query Release build: passed.
- Query CTest: 7/7 passed in 12.33 seconds from the committed final HEAD after MCP error/pagination hardening.
- The MCP Worker integration test covered Fresh, Connect, and Reconnect frame/zone/source comparisons, asynchronous validation/job retrieval, resources, and corrupt-trace rejection.
- Profiler Release build against the same `TracyAnalysis` and `TracyServer` sources: passed.
- Capture Release build: passed; its no-argument usage path exited with the expected nonzero status.
- Csvexport Release build: passed; `--help` exited successfully and an actual Fresh1 export completed successfully in 73.25 ms with output discarded.
- Original packaged `windows-0.13.1\tracy-profiler.exe` SHA-256 remained `1C7D6321E602B6A0B94A0897B70183AB62D4D56543C10ACD9BFC1B0424B48A78`.
- `git diff --check`: passed; only the repository's existing LF/CRLF conversion notices were emitted.

Final Windows artifacts:

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| `tracy-query.exe` | 11,970,560 | `CC0F36025FC874792A970CB8625DF755828ABF14C569D17283C87EB32167E60D` |
| `tracy-profiler.exe` (`GitRef=b5bab281`) | 26,558,976 | `9B015F990C599E36E3D194AAC7B228BF65057A63EB7B7ED754A8ADE8F88D654C` |
| `tracy-capture.exe` | 10,698,752 | `D75034A9A8A179BB23A86396E78D0ADCE84D6D3DC5719D1577824D7BB3DD4360` |
| `tracy-csvexport.exe` | 10,579,456 | `2CF5EB240988419B4578C316937FC08DC272E4929241516E18AD2AC371FD6C1C` |

## Profiler visual regression

The formal Profiler loaded `Test-GPU-Reconnect-1.tracy` directly from its native file argument, displayed 986 frames and the expected GPU/CPU/memory plots, and opened the shared frame-memory inspector without an error.

`Test-GPUMemory-Fresh1.tracy` exposed all six named D3D12 pools. Selecting `GPU D3D12 Default Textures` showed logical allocation IDs, 95 active allocations and 177.59 MiB. At frame 7,574 (internal index 610), the shared snapshot reported 177.59 MiB/95 allocations at both frame start and end, zero allocations/frees during the frame, and populated the allocation-detail table.

None of the ten available positive local captures contains persisted GTMEM1 relation records. Both Query (`CAPABILITY_UNAVAILABLE`) and the Profiler (`No GTMEM1 pass relations in this capture`) report that absence explicitly rather than inventing attribution. Deterministic request/pass/use pairing, incomplete passes, usage masks and ambiguity states are covered by the shared-analysis and `FakeTraceSource` contract tests; a future capture produced by a pass-attribution-enabled Godot build is still required for real-data GTMEM1 visual characterization.

## Real trace loader matrix

Times below are observed cold-process wall times and are characterization measurements, not hard cross-machine guarantees. Counts are read from `trace.overview` after `Worker::IsBackgroundDone()` and analysis indexing complete.

| Capture | Result | Time (s) | Fingerprint prefix | Frames | CPU zones | GPU zones | Memory events |
|---|---:|---:|---|---:|---:|---:|---:|
| Test-GPUMemory-FreshLive.tracy | ready | 0.134 | `a900` | 1,702 | 106,628 | 0 | 29,816 |
| Test-GPUMemory-Live.tracy | ready | 0.156 | `a13f` | 2,889 | 180,920 | 0 | 49,543 |
| Test-GPUMemory-Fresh1.tracy | ready | 0.139 | `ec9b` | 1,219 | 77,254 | 58,272 | 648,288 |
| Test-GPUMemory-Fresh2.tracy | ready | 0.137 | `2aed` | 1,211 | 76,708 | 57,936 | 643,664 |
| Test-GPUMemory-Connect1.tracy | ready | 0.142 | `015b` | 1,223 | 77,482 | 58,512 | 650,439 |
| Test-GPUMemory-Connect2.tracy | ready | 0.154 | `aabd` | 1,219 | 77,216 | 58,320 | 647,924 |
| Test-GPU-Reconnect-1.tracy | ready | 0.356 | `75aa` | 986 | 79,065 | 48,212 | 2,492,438 |
| Test-GPU-Reconnect-2.tracy | ready | 0.289 | `2195` | 1,109 | 112,188 | 55,981 | 1,905,094 |
| Test-GPU.tracy | ready | 1.468 | `f378` | 7,099 | 891,635 | 411,534 | 9,024,815 |
| Test-root.tracy | ready | 2.920 | `fdd3` | 10,435 | 340,957 | 167,496 | 4,947,284 |

Observed peak working set was 253.8 MiB and 243.9 MiB for the two Reconnect captures, 1,286.9 MiB for Test-GPU, and 2,276.3 MiB for Test-root. `Worker` storage is intentionally outside the 512 MiB derived-analysis cache budget.

## Corrupt Startup capture

`Test-GPUMemory-Startup.tracy` is not a valid positive acceptance capture. Its persisted `thread_pid_map` declares 3,023,656,976,384 records, while the file reaches EOF after only a small fraction of that declaration (the observed progress counter was about 2.62 million).

The untouched Tracy 0.13.1 `tracy-csvexport` did not terminate within 60 seconds on this file. The v1 loader now tracks the actual final decompressed block length and rejects reads past EOF. The same capture returns `CORRUPT_TRACE` with message `trace ended before all declared records could be read`, exit code 3, in approximately 0.358 seconds. It is used by CTest as a fail-fast regression fixture.

## MCP latency and binary resource checks

Measured with a persistent `tracy-query.exe --mcp` process and `Test-root.tracy`:

| Operation | Observed latency |
|---|---:|
| process start plus `initialize` | 91.11 ms |
| `initialize` call | 62.33 ms |
| `tracy_trace_open` acknowledgement | 0.60 ms |
| queued/loading/indexing to Ready | 2,668.63 ms |
| Ready `tracy_trace_status` | 0.26 ms |
| `tracy_describe` | 0.22 ms |
| first warm `tracy_overview` | 0.94 ms |
| second warm `tracy_overview` | 0.91 ms |

The first frame-image resource was read through MCP URI `tracy://trace/trace-1/frame-image/0`. It produced a valid, visually checked 320x180 PNG of 24,114 bytes with SHA-256 `1ab6735d2a6de4e1ae86494339f421767dcf05c045a77d7afb28dfa2bff74716`. Orientation, colors, and content were correct.

## Reconnect boundary finding

The Reconnect-1 capture contains 12 CPU zones whose persisted dynamic-name indexes are absent from the string table. Native unchecked name lookup could dereference one of these invalid indexes during a full zone comparison. `WorkerTraceSource` now validates string and zone-extra indexes, falls back to the source-location name, returns `name_resolved=false`, and reports `UNRESOLVED_CPU_ZONE_NAME` from `validation.run` with the affected zone refs. The real MCP validation job completed in 13.29 seconds and returned all 12 refs; the Reconnect zone comparison is retained as the regression test.
