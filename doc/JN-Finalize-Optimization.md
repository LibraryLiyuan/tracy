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
