> 历史归档：mainline f49190f2541c 中的历史资料。当前版本与操作入口见 [文档首页](../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# Offline string storage (P5 string checkpoint)

Offline conversion now preserves first-seen 32-bit string IDs without retaining the string content hash table and all string bytes in Worker memory. Full/live loading keeps the original `stringData`, `stringMap` and slab model.

The offline store uses three layers:

- Up to 256 KiB / 4,096 initial strings use charged stable heap allocations. This avoids fixed temporary-file overhead for small captures while keeping every returned address valid.
- Larger content is appended to stable file-mapped chunks. Existing mappings are never moved; chunks begin at 32 KiB and grow up to 16 MiB, while a single oversized string receives a dedicated aligned chunk.
- A file-mapped open-addressing hash index locates the current collision-chain head. Entry rows contain full length, location and previous candidate ID. Every hash match compares the complete bytes before treating values as equal.

The hash index grows through a separately accounted candidate, rehashes all occupied slots, flushes it, removes the old owned file and publishes the replacement. Key 0 and `UINT64_MAX`, updates, missing values, multiple resizes, sealed checksum validation and disk-budget refusal are covered. Interrupted indexes are not resumed.

Worker `StoreString`, lookup and indexed reads use the backend only in OfflineConvert mode. The in-memory content hash remains empty. Mapped addresses keep existing short-lived pointer contracts valid during replay; the output format does not persist those addresses. Before sealing, pointer-valued client/thread/external names are interned, then ordinary `.tracy` output uses deterministic `string ID + 1` tokens.采集协议中的地址 token 保持原字段语义，与文件内 token 分离。

Tests cover empty strings, forced hash collisions, duplicates, a 100,000-byte string, stable pointers across 5,000 inserts and index resizes, insertion order, hash, sealing and resource refusal. Worker roundtrip migrates a loaded dictionary to the backend, clears its memory dictionary, verifies duplicate/new IDs, writes an ordinary trace and compares every reopened string.

The final 64,891-byte full-flow fixture contains 118 strings and 1,307 logical bytes. It completed validation/publication/cleanup with private peak 83,128,320 bytes, workspace peak 229,911,952 bytes and disk peak 135,168 bytes under the 194,673-byte 3J limit. No mapped content chunk was needed for this small fixture. Global-string streaming validation, callstack/symbol dictionaries and the other remaining P5/P6 holders are still pending; the 30-minute acceptance has not started.
