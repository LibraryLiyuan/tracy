# Offline Replay and Write optimization

The long-data converter now reports Replay transport waits, payload read time, sampled event costs, storage-cache activity, Write domain timings, compression-worker time, main-thread compression wait, and physical output writes. These counters are collected at record, block, or 65,536-row boundaries; process and system memory remain sampled by the existing 100 ms monitor.

Replay storage reuses bounded cache slots instead of releasing and reacquiring workspace reservations on every miss. `MutableRows` retains its 16 KiB page format and checksum. `OrderedForest` uses one workspace-charged row-sized scratch buffer instead of clearing and copying 4 KiB stack rows for 62/82-byte CPU/GPU records. The ResourceSet B+ tree performs lookup and insertion on cached pages, uses O(1) LRU eviction, avoids unchanged parent writes, and reuses its page slots. ResourceSet definitions reuse one bounded member buffer.

Paged record writers and readers reuse a shared, budgeted Zstd scratch arena. Active ResourceSet body reads retain their data and directory handles, flush dirty output only when required, and reuse decoded cache slots. Every block remains independently checksummed, decoded, and included in the complete logical hash.

Snapshot `FileWrite` checks cancellation/failure at 64 KiB block boundaries rather than for every scalar field. Compressed length/payload records are coalesced into a 1 MiB physical output buffer. Disk growth is still reserved before each actual write; short write, worker failure, finish, flush, and close errors remain fatal. The 64 KiB compression format and stream ordering are unchanged.

Write now generates canonical CPU/GPU Zone semantic hashes while traversing the final order. Reopen validation independently checks sequential indices, parent/child nesting, declared child counts, thread/context, times, source location, ZoneExtra/callstack references, and GPU query IDs. Sample hashes are generated in 4,096-row chunks during the Write traversal and reused by Validate, avoiding a second source-store scan.

## Fixed component measurements

Each configuration was run once in Release using the same deterministic fixture. Baseline is commit `1c0eb5ef`; candidate includes commits `23872155` and `4ff5a1d5`.

| Case | Baseline | Candidate | Change |
|---|---:|---:|---:|
| OrderedForest 500,000 rows, build | 0.106461 s | 0.099128 s | 6.9% faster |
| OrderedForest 500,000 rows, visit | 0.118774 s | 0.078720 s | 33.7% faster |
| ResourceSet 300,000 definitions, build | 0.849560 s | 0.152748 s | 82.0% faster (5.56x) |
| ResourceSet 100,000 cold reads | 23.554062 s | 16.257326 s | 31.0% faster (1.45x) |
| Paged records 512 MiB, build | 1.253310 s | 1.123415 s | 10.4% faster |
| Paged records 512 MiB, validate/visit | 0.834998 s | 0.810681 s | 2.9% faster |
| FileWrite 256 MiB as 67,108,864 scalar calls, 27 streams | 0.268163 s | 0.248258 s | 7.4% faster |

The scalar FileWrite output size was identical (268,447,906 bytes). The baseline physical-write count was not instrumented; its implementation issued a length and payload write for each of 4,096 compression blocks plus three header writes. The candidate measured 258 physical writes.

For the fixed 256 MiB block-input comparison, 8 streams took 0.162972 seconds and 27 streams took 0.171866 seconds. This 5.2% difference is below the plan's 10% decision threshold, so the converter's thread policy is unchanged. The output-size difference was 114 bytes.

These measurements establish local effects only. They do not predict the final 30-minute Replay, Write, Validate, or total time. A single frozen real-input run remains required.

## Verification

Thirty-two Tracy component tests were run. Twenty-nine passed through CTest; three fixed-output-directory tests encountered prior artifacts, and all three passed when rerun once with fresh explicit directories. Offline Worker disk/full equivalence, maximum ResourceSet expansion, CPU/GPU tree round trips, Zone field/order validation, Sample ordering and hash reuse, file read/write failure propagation, and the small complete Scan-to-Cleanup conversion pass.

The resource contract remains 20 GiB process private bytes, 256 MiB shared storage workspace, 2 GiB commit/free-space reserves, and 3J disk peak. No Trace layout, stream protocol, Unity path, Query/MCP 1.35 contract, or retry policy changed.

## First real-input run

The frozen r6 run completed Scan in 40.979 seconds, Replay in 948.018 seconds, and Finalize in 84.304 seconds. Relative to r4, Replay fell from 3,424.24 seconds to 948.018 seconds (72.3% reduction, 3.61x throughput) and Finalize fell from 132.449 seconds to 84.304 seconds (36.4% reduction). The run then stopped 92.079 seconds into Write with `offline_sample_visit_count`, after 85,393,408 of 422,576,574 CPU Zones had been emitted. A full Write comparison cannot be claimed from this partial result.

Replay evidence shows 30.349 seconds of client payload reads and 915.290 seconds waiting for the one-record mailbox to become empty. This identifies Worker processing/storage as the Replay limiter; journal input is not the dominant cost. The optimized storage reused 2,195,077 CPU-page slots after 125 allocations and 186,129 GPU-page slots after 125 allocations. ResourceSet performed 21,655,059 lookups with 20,651 page-slot reuses.

The run remained within all resource limits: private bytes peaked at 3,535,556,608, workspace at 212,287,527, and task disk at 67,128,279,040 bytes. Cleanup completed, retained 36,864 bytes of reports, and published no candidate, final trace, map, or completion marker.

The failure exposed a pre-existing Sample ordering defect. `OfflineSampleStore::State::lastTime` tracked the last appended time rather than the maximum observed time. After a sequence such as 10, 5, 10 (or a zero-time sample), the later duplicate bypassed lookup; the time index replaced the previous key while the declared count increased. Commit `9675e6ff` keeps the maximum time, so every later timestamp at or below that maximum is checked and duplicate timestamps are merged. The 10, 5, 0, 10 regression and the complete small conversion both pass. Per the one-run stop rule, the 30-minute input was not rerun automatically.

## Sample fix acceptance

Commit `8ca23514` adds a constant-time guard against replacing an existing indexed main-Sample timestamp and preserves append order for context-switch Samples. The storage tests observed both failures before the production changes. The Worker regression now processes distinct A, B, A callstacks across time rollback and zero-time rows, verifies the exact six-frame merged stack, writes the Trace, reopens it, and completes streamed validation. Query 1.35 resolves the resulting Sample and callstack content.

The frozen r8 converter completed the 26,193,953,055-byte input in 1,864.710 seconds. Replay took 1,067.32 seconds, Finalize 83.498 seconds, Write 486.535 seconds, and Validate 72.144 seconds. The published Trace is 12,428,310,213 bytes. It contains 114,809,633 Samples, 25,083 fewer than the invalid r6 declared count, and passed sequential write and reopen hashes.

Private bytes peaked at 3,549,048,832, the shared workspace at 212,287,527, and task disk at 74,638,065,664 bytes. Cleanup removed the work tree and candidate; the snapshot map and completion marker identify the published r8 output.

The frozen r8 Query could not open the 12.43 GB Trace: it returned `RESOURCE_LIMIT: not enough memory to load trace` after `WorkerTraceSource` translated a `std::bad_alloc`. Converter long-data acceptance is complete; Query long-data loading remains a separate implementation requirement.
