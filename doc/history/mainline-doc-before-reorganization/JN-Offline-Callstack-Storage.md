> 历史归档：mainline f49190f2541c 中的历史资料。当前版本与操作入口见 [文档首页](../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# Offline callstack storage (P5 callstack checkpoint)

Offline conversion now stores unique callstack frame-ID arrays outside the Worker slab and in-memory content hash. Full/live behavior is unchanged. The existing `callstackPayload` vector retains only the reserved null entry and one compact `VarArray` descriptor per unique callstack, so current consumers and the ordinary Trace writer continue to use the same logical IDs.

The backend uses the same bounded model as the string dictionary: a charged 256 KiB / 4,096-entry small tier, stable mapped chunks after the threshold, a mapped open-addressing hash head index and fixed mutable entry rows. Hash candidates are accepted only after comparing the full frame-ID array. Entry IDs start at 1 and are checked against the unchanged 24-bit callstack limit before publication.

Native payloads, allocation callstacks and merged samples all route through one Worker helper. A new unique body receives the next original ID and a small `VarArray` descriptor; duplicates reuse the existing ID. Frame queries retain the prior first-definition behavior. Temporary merge buffers are bounded by the existing 65,535-frame `VarArray` representation.

Tests cover forced hash collisions, duplicate IDs, stable bodies, 64-frame content, 5,000 unique arrays across index resizes, visit order, sealing/hash and disk-budget refusal. The Worker roundtrip adds and deduplicates a two-frame native callstack, writes an ordinary trace, reopens it and verifies canonical frame addresses; all prior string, CPU/GPU Zone, Catalog, Job/Use and maximum-set cases continue to pass.

The 64,891-byte full-flow fixture contains no callstack payload records, so it validates the empty-table path. It completed conversion, validation, publication and cleanup with private peak 83,116,032 bytes, workspace peak 229,912,072 bytes and disk peak 135,168 bytes under the 194,673-byte 3J limit. Callstack frame/symbol dictionaries, Sample history and global streaming validation remain pending. The 30-minute acceptance has not started.
