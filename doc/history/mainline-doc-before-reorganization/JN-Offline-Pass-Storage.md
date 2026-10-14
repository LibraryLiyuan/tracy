> 历史归档：mainline f49190f2541c 中的历史资料。当前版本与操作入口见 [文档首页](../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# Offline Pass storage (P2)

OfflineConvert stores GfxEntity, GfxLink and GpuReferencePass in the existing compressed append-table backend. The normalized fields and original ordering are retained. Full/live readers and ordinary Query consumers keep their existing behavior. Serialized Reader domain ordinals remain stable because Query persists the first seven section identities.

The task-local 64-bit B+ tree uses 16 KiB pages, a 4 KiB file header, an 8 MiB cache (4 MiB under pressure) and a separately charged 1 MiB traversal allowance. Every file extension is charged to ConversionResources. The tree is built lazily after replay/seal, published to the storage context only after successful sealing, and reused across Catalog generations. Interrupted trees are not resumed.

Records are applied in source order: Reference Pass, explicit GPU Pass entity, then RangeEvidenceComplete link. A later record in a source overwrites an earlier record even if its timestamp is smaller. Zero IDs and other entity/link kinds do not enter the index. Signed times retain their bit representation. Original fact tables are never deduplicated.

Tests cover 5,002 sparse keys including UINT64_MAX, splits and two-page eviction, overwrites, missing keys, workspace accounting, budget refusal, cancellation and corrupted pages. Worker fixtures compare all normalized fact fields and order, high Pass IDs, negative times, source precedence, unrelated links, and two generations resolving the same pointer across disjoint resource lifetimes. Full and disk paths select the same resource. New-format write, chunked verification and full reopen pass. A small stream conversion completes publication and cleanup.

P2 does not externalize Catalog resource intervals or other Catalog records, CPU/GPU zone objects, or global string/callstack dictionaries. Those remaining holders prevent a bounded-memory claim for the entire converter. No additional 30-minute performance round has been started.
