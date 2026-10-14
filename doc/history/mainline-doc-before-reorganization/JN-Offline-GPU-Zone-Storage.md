> 历史归档：mainline f49190f2541c 中的历史资料。当前版本与操作入口见 [文档首页](../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# Offline GPU Zone storage (P4 completion)

Offline conversion now stores completed GPU Zone history in the same page-backed ordered forest model used by CPU Zones. Each normalized 42-byte row contains CPU start/end, GPU start/end, source location, callstack, compressed thread and query ID. Context/thread roots retain the existing ordering and nesting semantics, while Full/live Worker paths keep their original timelines and slab allocation.

An active `GpuEvent` proxy exists only while the Zone is on a context stack or waiting for a GPU timestamp query. Begin and end query IDs continue to use Tracy's per-context query table. Each CPU or GPU timestamp mutation is written to the stable logical row. After the second GPU timestamp completes the record, the no-statistics converter recycles the proxy. Unknown JN callsites retain the logical row ID, so a definition arriving after proxy release can still update the final callstack safely.

Output streams the forest directly in ordinary Trace order: CPU/GPU start deltas, source location, 24-bit callstack, thread, 64-bit child count, CPU/GPU end deltas and query ID. Root child counts and total branch counts are tracked during construction, so write and reopen do not reconstruct the full tree or recurse through the nesting depth.

The storage test covers multiple roots, mutable CPU/GPU timestamps, callstack updates, root counts and ordered traversal. The Worker fixture covers nested zones, four query IDs, independent serial/thread CPU clocks, both GPU timestamps, delayed callsite enrichment after proxy release, ordinary `.tracy` write and full reopen. CPU/GPU combined native regressions and the statistics-enabled Query build passed.

The final 64,891-byte P4 full-flow run stored 218 CPU Zones (one thread, 110 child lists) and zero GPU Zones from that source fixture. It completed validation, publication and cleanup with private peak 83,038,208 bytes, workspace peak 229,514,864 bytes and disk peak 118,784 bytes under the 194,673-byte 3J limit. The report now exposes CPU/GPU Zone records, roots, branches and temporary logical bytes. The integrated 30-minute acceptance remains deferred until P5/P6 complete.
