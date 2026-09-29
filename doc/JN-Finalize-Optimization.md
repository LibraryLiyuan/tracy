# Offline Finalize optimization

The offline converter exposes bounded Finalize substages for global string finalization, ordinary store sealing, Catalog batch-directory construction, Pass-time construction and sealing, Catalog generation resolution, batch checksums, and Catalog sealing. Progress reports task disk current/peak/limit and shared-workspace current/peak/limit. A sibling `.cancel` file (or explicit `--cancel-file`) requests cooperative cancellation; cancellation follows the normal report and owned-file cleanup path.

GPU Catalog batch lookup uses a temporary page-backed directory built once after replay. A generation index maps sparse 64-bit generation values to disk directory rows. Per-kind reference chains preserve accepted batch order and support interleaved generations. Traversal rechecks generation, kind, validity, record range, chain length, and reference bounds. The directory is removed after Catalog sealing, before ordinary Trace write and validation.

Pass-time lookup is prepared explicitly on the first unresolved RangeSet. RangeSets that already contain a resource ID do not build the index. The three existing precedence passes remain unchanged: GPU Reference Pass, ExplicitGpuPass entity, then RangeEvidenceComplete link, with later rows in one source overwriting earlier rows. The equality-only index uses the existing disk-accounted mapped hash. It is sealed before queries and removed after Catalog sealing.

The shared B+ tree and mutable-row stores use O(1) LRU victim selection. B+ tree traversal no longer copies complete 16 KiB pages, and unchanged updates do not dirty the path. Ordered/range uses such as resource lifetimes retain the B+ tree. Append-domain readers validate block continuity, block checksums, decoded content, and the complete content hash while delivering each decoded block once.

Resource lifetime reporting records additions, queries, visited interval nodes, and the maximum nodes visited by one query. The bounded result cache proposed during review was not added: the fixed short test performed 67,108,864 interval-node visits in 0.683 seconds, so the available evidence did not justify its semantic and memory cost.

## Fixed short measurements

All measurements used the Release build, fixed deterministic inputs, and one run per configuration.

| Case | Result |
|---|---|
| B+ tree, 1,048,576 equality keys | build 76.163 s; 65,536 queries 3.831 s; disk peak 31,756,288 B |
| mapped hash, 1,048,576 equality keys | build 0.259 s; seal 0.145 s; queries 0.009 s; disk peak 75,505,664 B |
| mapped hash, 4,194,304 equality keys | build 0.794 s; seal 0.590 s; queries 0.026 s; disk peak 301,998,080 B |
| batch directory, B=65,536/G=1 | build 0.0062 s; visit 0.0008 s |
| batch directory, B=65,536/G=128 | build 0.0078 s; visit 0.0013 s |
| lifetimes H=1,024/Q=8,192 | 8,388,608 node visits; query 0.0757 s |
| lifetimes H=8,192/Q=8,192 | 67,108,864 node visits; query 0.6830 s |
| validated paged visit, 128 MiB | 0.539 s; 134,217,728 decoded bytes exactly once |
| validated paged visit, 512 MiB | 2.162 s; 536,870,912 decoded bytes exactly once |

The 4,194,304-key B+ tree case was not started because the 1,048,576-key run consumed 76 seconds and its projected duration exceeded the fixed 180-second short-test window. The mapped-hash result justified changing only the equality-only Pass index; it does not replace ordered indexes.

These measurements are local component evidence. They do not establish the 30-minute conversion time, the final 3J peak, or Query long-data acceptance.

## Real-input correction after the first acceptance attempt

The first 30-minute acceptance attempt reached the new mapped Pass index with 193,409,512 source rows and 52,532,087 unique keys. Replay completed in about 3,822 seconds, the Pass index build completed in about 83 seconds, and the 3,221,229,568-byte index stayed near 3.09 GiB process private bytes. The run was stopped when `pass_time_seal` reached the previous 113-minute Finalize stop baseline without completing. Task disk peaked at 67,123,949,568 bytes and workspace at 204,599,959 bytes, both within their fixed limits. Owned temporary files and the candidate output were removed.

The cause was isolated to the Windows streaming SHA-256 path used for the complete mapped-index digest. The implementation now uses Windows CNG while preserving the SHA-256 bytes, processes the mapping in 32 MiB cancellation points, and reports `pass_time_digest` in bytes separately from `pass_time_flush`. Pass build progress no longer depends on block-size alignment. The resource evidence records digest bytes, digest time, flush time, and the hash backend.

A single same-scale component run used exactly 52,532,087 keys and a 3,221,229,568-byte final index. It built in 11.457 seconds, digested in 1.276 seconds, flushed in 1.148 seconds, and sealed in 2.424 seconds with the `windows-cng` backend. The benchmark directory was deleted after the result was captured.

Cancellation evidence was also made independent of the ordinary failure state: the final `Cancelled` progress document is written before the resource report, owned-file evidence is bounded to 64 path entries with aggregate counts and bytes, and the control-file reserve is 1 MiB. This corrected the real attempt's `conversion_control_budget` failure while retaining the 3J accounting contract.

The next real-input retry exposed an observability failure rather than a data-path failure. `catalog_directory` can publish more than one hundred progress updates in under a second. Each update woke the reporter immediately, so Windows metadata replacement eventually returned access denied while another system component briefly held the progress file. The converter had completed Replay in 3,213.7 seconds and ordinary store sealing in 48.4 seconds; it then stopped at 39.8% of the directory build. Cleanup completed, with a 62,199,853,056-byte disk peak, a 3,518,693,376-byte private peak, and a 204,611,911-byte workspace peak.

Progress JSON is now emitted on a fixed two-second cadence regardless of producer notifications. Windows control-file replacement retries access-denied, sharing-violation, and lock-violation errors for a bounded 550 milliseconds. A progress-only write failure increments `progress_write_failures`, emits one warning, and retries at the next interval without failing valid conversion work. Final reports, snapshot maps, and completion markers remain fatal when they cannot be published. A deterministic regression holds the existing metadata file without delete sharing for 100 milliseconds; the replacement now succeeds after retry.

## Real-input Catalog-resolution correction

The next real run passed Replay, ordinary store sealing, Catalog-directory construction, and the 52,532,087-key Pass-index build. The Pass build took about 25 seconds and the same-scale digest/flush remained seconds-level. The progress label then stayed at completed `pass_time_flush`, but source inspection and temporary-file sampling proved that the flush had returned: the worker was resolving Catalog rows without restoring a domain-specific progress phase.

The input contains 10,308,399 resource records, 2,528,887 resource-lifetime intervals for 41,930 pointers, 9,570,343 RangeSet rows, and 11,451,747 relation rows. The lifetime chains reach 756 nodes. `ResourceIntervals` previously served every chain node through a 2 MiB `MutableRows` page cache. Each cache miss read a 16 KiB page and recomputed its SHA-256 checksum. RangeSet and relation resolution therefore amplified millions of logical lookups into repeated page reads and hashes. At 20,013.9 seconds total elapsed, only about 1.72 million of the 11.45 million relation rows had cleared their pointer flags. The run was classified as a performance failure and stopped; 65,517,109,840 bytes of task-owned temporary data were removed, and no candidate, final trace, or completion marker was published.

Resource lifetime rows now use a 4,096-row, 128 KiB workspace-charged small tier followed by directly addressable file-mapped chunks. Handles preserve append order and the low open-state bit; mapped reads validate handle bounds but do not perform a page read and SHA-256 for every chain node. The files are task-local, are never resumed, remain disk-accounted, and are removed at the end of each generation. Cancellation is checked at every lookup and every 1,024 visited chain nodes. Catalog resolution now reports resources, views, logicals, virtual geometry, RangeSets, relations, and detailed evidence separately, with updates throttled to 65,536 rows.

One mapped-path component run used a 5,000-node history and 100,000 queries: 500,000,000 node visits completed in 1.891 seconds with a 3,316,040-byte workspace peak. A small-tier run performed 756,000,000 node visits in 2.555 seconds. Resource-interval semantics, worker disk/full equivalence, mapped-hash/string/callstack regressions, and the small complete conversion all pass. The small conversion also verifies the new storage evidence, 3J accounting, publication, reopen validation, and cleanup. These component results justify another single real acceptance run; they do not by themselves establish the final 30-minute result.

## Fourth real-input run and validation-reader correction

The fourth run completed the entire optimized Finalize stage. Scan took 39.736 seconds, Replay 3,424.24 seconds, Finalize 132.449 seconds, and Write 1,501.34 seconds. Finalize processed 193,409,512 Pass-time source rows into 52,532,087 unique keys; the 3,221,229,568-byte index built in 23.336 seconds, sealed in 2.507 seconds, digested in 1.279 seconds, and flushed in 1.228 seconds. Catalog resolution completed 24,449,175 lifetime lookups and 635,470,383 interval-node visits, including all 9,570,343 RangeSets and 11,451,747 relations. This establishes that the former multi-hour Finalize obstruction is removed on the 30-minute input.

The run then failed 1.140 seconds into Validate before reading snapshot records. The candidate was 12,333,805,568 bytes. Peak task disk was 74,543,955,968 bytes, private bytes 3,519,356,928, working set 10,991,722,496, and shared workspace 204,611,911 bytes; all remained within the fixed 3J, 20 GiB, and 256 MiB limits. Cleanup removed the candidate and all large temporary files, retained 28,672 bytes of reports, and published no final trace, map, or completion marker.

The failure was a validation-reader accounting defect. `FileRead` reserved a hard-coded 8 MiB for each of the 27 compression streams, charging 216 MiB to the same shared workspace while the conversion storage was still alive. Progress evidence shows only 3,279,832 workspace bytes were live before this reservation. The reader now accounts two 64 KiB buffers, the stream handle, the bounded Zstd decoder estimate for its configured 4 MiB maximum window (or the LZ4 decode state), and a 1 MiB per-stream thread/allocation allowance. Multiplication and addition are checked for overflow. Runtime budget failures retain their original reason instead of being replaced by `file_read_open_or_decode_failed`.

A 32-stream Zstd round-trip now succeeds under a 192 MiB reader cap; the old fixed formula requested 256 MiB. The complete small conversion again passes Validate, publication, reopen, 3J accounting, and cleanup. Worker storage semantics and ResourceIntervals tests pass, and Query opens the resulting JNTRACYX v1 trace and returns schema 1.35 `trace.overview` with 32-bit string indexes.

The fourth run is a successful real-input Finalize performance result, but it is not a successful end-to-end conversion acceptance because Validate failed and no real output was published. Under the one-run and stop rules, another 30-minute run was not started automatically. The validation-reader correction therefore has component and small end-to-end evidence; a future explicitly scheduled real run is still required for final Validate, Query, Publish, and Cleanup acceptance.
