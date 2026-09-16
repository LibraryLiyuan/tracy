# Catalog storage progress (P3)

Implemented first: resource lifetime resolution uses a task-local disk index and fixed-width mutable rows. Offline Worker no longer fills its local `intervals` and `openIntervals` history maps. Full/live retain the original algorithm. Each generation's temporary lifetime files are removed when resolution exits, including failure unwinding; unsuccessful deletion remains charged and latches a conversion failure.

The pointer index stores a checked temporary interval ID plus one open/closed flag. The current open interval, when present, is always the latest interval, so no separate state table is needed. Interval records contain resource ID, inclusive begin/end timestamps and the preceding interval ID. The chain is monotonic and range-checked; lookup uses bounded memory even for one pointer with long history. Temporary IDs are range-checked before encoding; this does not change any persisted Trace field.

Mutable pages are 16 KiB with schema/width/page identity checks and payload checksum. B+ tree pages remain 16 KiB; their task-local file header is 4 KiB (`JNIDX642`). Interrupted temporary files are not resumed. Page caches, fixed I/O allowance, index traversal and registry allocations use the shared workspace budget; all files use conversion disk accounting.

The original implementation's Create/Open/Snapshot/Destroy/update fallback, mismatched Destroy, inclusive boundaries, gaps and ambiguity semantics are retained. Unit tests include 10,000 successive lifetimes for one 64-bit pointer. Worker tests compare two generations against the unchanged memory path and preserve Pass source priority.

A 64,891-byte fixture initially hit the 3J guard because the first design duplicated pointer state and used larger file headers. Removing that duplication and reducing the header restored full conversion without increasing the 194,673-byte limit. Verified peak was 122,880 bytes; publication and cleanup completed. This is a small functional fixture, not the requested 30-minute acceptance.

Reports retain peak interval row/pointer counts and logical file bytes before per-generation cleanup. The global resource report remains authoritative for cluster-rounded total disk usage.

Still pending within P3: Catalog fact columns, batches/generations/controls, strings, finalized checksums and explicit streaming validation. CPU/GPU zone/slab and global dictionary work also remain. The first P3 step does not establish bounded memory for the whole converter.
