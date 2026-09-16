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

Query index schema is 15. Analysis cache namespace is `query-analysis-cache-v2-jntracyx1-string32`; decoder identity is `jntracyx-v1-base-0.13.2-string32`. Old caches remain on disk but are not reused. Query API version remains 1.35. Trace metadata and capabilities identify the new format separately from program and base-layout versions.

## Validation status (2026-09-16)

Passed: header / index codec boundaries, mutable 32-bit ZoneExtra roundtrip, canonical output hashing, existing relation / mutable / record-reader regressions, safe snapshot failure preserving an existing file, small stream conversion through validation / publication / cleanup, and Worker Full versus external storage field and order comparison with 65,535-member resource sets.

Query and Profiler integration builds and further fault tests are in progress. No 30-minute acceptance has been run for this format. Catalog, zone/slab and dictionary storage work remains; format capacity does not establish bounded process memory. Final integrated acceptance remains one round with soft 16 GiB / hard 20 GiB, shared workspace 256 MiB and added disk 3J.
