> 历史归档：mainline f49190f2541c 中的历史资料。当前版本与操作入口见 [文档首页](../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# JN extended Trace v1

The compressed container is unchanged. The decompressed header is exactly 24 bytes: ASCII `JNTRACYX`, little-endian u32 version 1, u32 base layout 0xD02, and u64 features 1. Readers reject every other combination before loading historical records. The `.tracy` suffix does not imply upstream compatibility. Stream capture protocol is unchanged.

`StringIdx` is four little-endian bytes containing index + 1; zero is inactive. Maximum index is 4,294,967,294. Encoding checks a 64-bit input before narrowing. Other Int24 fields retain their widths.

## Persistent field inventory

| Owner | StringIdx fields | Encoding / read-write contract |
|---|---|---|
| ZoneExtra | text, name | 14 bytes: callstack 3, text 4, name 4, color 3 |
| LockMap | customName | field encoding, sizeof(StringIdx) = 4 |
| CallstackFrameBasic | name, file | packed size 12 including line |
| CallstackFrame | inherited name, file | packed size 20 including symbol address |
| CallstackFrameData | imageName | field encoding, four bytes |
| SymbolData | name, file, imageName, callFile | packed size 28; field-based file operations |
| GpuCtxData | name, noteNames values | field encoding, four bytes |

SourceLocation and Message use StringRef (nine bytes) and are unchanged. SymbolPending is a runtime structure; its StringIdx follows the same four-byte representation. Reader validation rejects active indices outside the loaded dictionary.

## Mutable ZoneExtra storage

Temporary records contain four u32 values (16 bytes). Pages use schema 2 and recordBytes 16; incompatible temporary files are rejected. Callstack and color are checked against 24 bits when encoding the final 14-byte record. Hashes cover final canonical bytes, not temporary padding. The default cache is capped by bytes at 32 MiB and reduced to 4 MiB under pressure, inside the shared workspace quota.

## Cache and tool identity

Query index schema is 15. Analysis cache namespace is `query-analysis-cache-v2`; decoder identity is `jntracyx-v1-base-0.13.2-string32`. The short directory name limits path growth; the full decoder descriptor is validated in the cache header. Old caches remain on disk but are not reused. Query API version remains 1.35. Trace metadata and capabilities identify the new format separately from program and base-layout versions.

## Validation status (2026-09-16)

Passed: header / index codec boundaries, mutable 32-bit ZoneExtra roundtrip, canonical output hashing, existing relation / mutable / record-reader regressions, safe snapshot failure preserving an existing file, small stream conversion through validation / publication / cleanup, and Worker Full versus external storage field and order comparison with 65,535-member resource sets.

Converter, Query, Profiler, capture, stream-inspect, update, CSV export, Chrome import and Fuchsia import build successfully in isolated directories. Query contract, analysis cache manager and cache table tests pass, including a dedicated long-path table roundtrip. Query reports unsupported legacy files as UNSUPPORTED_TRACE_VERSION. Native full-file fixtures cover lock names, callstack name/file/image, SymbolData strings and GPU name/noteNames. Sparse codec references exercise the high index boundaries without allocating millions of strings; full-file fixtures use real continuous dictionaries and reject missing references.

Independent old/new tools agree on 218 CPU zones including name, text, source, times and hierarchy, all messages, counts and frame statistics for the same small stream. Auxiliary resave followed by Query reopen preserves the zone fields. Profiler timeline, Zone name/text, Catalog overview and explicit legacy rejection were visually checked. Other persistent string fields were verified by native roundtrip fixtures; exhaustive GPU/lock/callstack UI inspection remains separate.

The real compact-index fixture exposed an existing shared-sink integration gap: Query must decline domains it does not own, including the base append domains, rather than throw from its seven-section layout switch. The guard is corrected; actual index build/validation/open tests preserve Job Stage, GfxLink and Reference Pass counts. Stable serialized domain ordinals are asserted separately from the growing offline storage registry.

No 30-minute acceptance has been run for this format. Catalog, zone/slab and dictionary storage work remains; format capacity does not establish bounded process memory. Final integrated acceptance remains one round with soft 16 GiB / hard 20 GiB, shared workspace 256 MiB and added disk 3J.
