# Offline Sample storage and validation

Offline conversion now removes regular and context-switch Sample history from `ThreadData` vectors. Full/live paths retain their original vectors and statistics behavior.

Each sample is normalized to 12 bytes (`time`, 24-bit-compatible callstack ID held in 32 bits) and appended to a page-backed linked list per thread and kind. Zero-time samples keep insertion order in a separate prefix list. Monotonic threads do not create a time index. On the first nonzero out-of-order or duplicate timestamp, the store indexes that thread's existing rows; duplicate timestamps reuse Worker `MergeCallstacks` and update the original row, while unique out-of-order rows are emitted in timestamp order. Context-switch samples retain their independent sequence.

The optional global time index encodes a bounded 17-bit internal thread/kind ordinal plus the existing 47-bit nonnegative timestamp. Disk B+ tree range traversal was added and tested to stream one ordinal's keys in sorted order without loading the tree. Thread state accounting is charged progressively to the shared workspace.

Tests cover interleaved threads, regular/context kinds, 10,000 monotonic records without a time index, duplicate callstack update, earlier timestamps, multiple zero-time rows, sorted visit, ordinary Trace write/reopen and a non-empty streamed validation pass. Validate compares each thread/kind count and SHA-256 over normalized rows while leaving both Sample vectors empty.

The 64,891-byte end-to-end fixture contains no samples and therefore verifies the empty-store/report path. It completed conversion, validation, publication and cleanup with private peak 83,144,704 bytes, workspace peak 229,911,952 bytes and disk peak 135,168 bytes under the 194,673-byte 3J limit. The report exposes total Sample records, threads and whether the disorder index was required. The integrated 30-minute acceptance remains pending.
