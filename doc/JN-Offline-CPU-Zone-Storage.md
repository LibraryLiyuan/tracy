# Offline CPU Zone storage (P4 checkpoint)

Offline conversion now keeps completed CPU Zone history in an ordered, page-backed forest. Full/live Worker behavior is unchanged. Each thread owns a synthetic root; every Zone row stores start/end, source location and ZoneExtra ID, while the forest stores first/last child, next sibling, parent and child count. Stable logical IDs replace history pointers at the storage boundary.

Construction uses mutable 16 KiB pages and a bounded cache. The first child of each parent increments the persisted child-list count used by the ordinary Trace header. Output traverses parent/first-child/next-sibling links iteratively, so neither writing nor validation needs a recursion stack or a reconstructed in-memory tree. Small histories can remain in the charged cache without creating a temporary file; eviction writes pages through the conversion disk ledger.

Active zones still use the existing `ZoneEvent` shape so stack checks, Zone text/name/color/value updates and source-location counting retain their protocol semantics. On end, the normalized row is updated and the active proxy is recycled in the no-statistics converter build. A delayed JN callsite definition stores a logical row ID rather than a `ZoneEvent*`, allowing callstack enrichment after the active proxy has been released.

Tests cover two interleaved roots, sibling order, a 10,000-level tree with constant-stack traversal, cache eviction, mutable updates, root/branch counts, delayed callsite enrichment after both nested zones end, ordinary `.tracy` write/reopen and all prior Worker semantic cases. A 64,891-byte end-to-end conversion completed Scan, Replay, Finalize, Write, Validate, Publish and Cleanup. It produced a 29,575-byte trace with 83,066,880-byte private peak, 229,514,864-byte workspace peak and 118,784-byte disk peak under the 194,673-byte 3J limit; cleanup reported no residuals.

GPU Zone history was completed in the following P4 checkpoint; see `JN-Offline-GPU-Zone-Storage.md`. Global string ownership and the remaining P5/P6 work are pending. Neither checkpoint is the integrated 30-minute acceptance.
