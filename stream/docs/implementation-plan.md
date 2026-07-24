# Tracy streaming implementation and acceptance plan

## Baseline

The work is integrated directly on `feature-StructuredData` in
`C:\CodeProjects\GodotProjects\tracy-0.13.1`. The temporary streaming worktree
was removed; Tracy now has one worktree and branch/commit history is the only
development isolation. Godot and TPS remain independent clean repositories.

Normal `.tracy` capture still uses a full `Worker`. Journal-only capture uses
the bounded `Worker::Mode::ProtocolOnly` resolver: it retains only definition
and protocol state needed to issue Tracy's bidirectional queries, not timeline
events.

Unreal Insights provides a useful backpressure model: socket reads are gated by
completion of bounded asynchronous file buffers, and live readers tolerate
temporary EOF. It does not provide an application ACK/retransmit or
power-failure commit format, so `.tracy-stream` adds explicit record integrity
and commit boundaries.

## Execution snapshot

Completed in the current implementation:

- single-worktree repository-state audit;
- v1 file/record/trailer contract;
- writer, scanner, explicit repair, resume, fault injection, and CLI;
- asynchronous strict two-buffer protocol observer with TCP backpressure,
  durable checkpoints, and explicit sink-error propagation;
- bounded ProtocolOnly resolver with memory, definition, and query-queue caps;
- replay-external v4 `LocalControl` record for the local `BeginDrain` boundary,
  including the original server-query window; native Client disconnect stops
  new producer events while the pre-disconnect backlog (finite at the boundary,
  but without a Client hard limit) and asynchronous definition responses drain,
  followed by ordinary `ServerQueryTerminate`;
- SessionBegin v2 records ProtocolOnly replay mode from session start, so live
  revisions before `BeginDrain` use the same symbol-expansion behavior;
- deferred optional symbol-code/disassembly expansion in ProtocolOnly and
  matching Full replay with the recorder drain completion predicate;
- ten-second per-server-record replay deadlines, verifier-triggered socket
  close on divergence, and a second-Ctrl+C recoverable force-stop path;
- offline replay with outgoing query-byte validation and `.tracy` conversion;
- immutable `JournalStore` read views with revision/watermark publication and
  committed-prefix fingerprint revalidation;
- revision-backed `SegmentTraceSource` and direct `.tracy-stream` MCP queries;
- live growth polling, direct/replayed MCP A/B, and real Godot/TPS acceptance;
- source-level Unreal Engine 5.7.2 Recorder/Store/TraceAnalysis audit.

The planned stages are complete. Live querying currently materializes each new
committed revision through strict local replay before atomically swapping the
query source. This favors correctness and API compatibility over incremental
analysis cost; native segment-domain indexes are a future optimization, not a
correctness blocker.

## Delivery stages

### Stage 1 — journal core and recovery

1. Freeze the v1 on-disk contract and single-session reliability semantics.
2. Implement portable append, CRC32C, commit trailers, publish/durable flushes,
   bounded scanning, explicit repair, and resume after a validated prefix.
3. Add fault-injection tests for short writes, write/flush failures, every
   truncation position, payload/header/trailer corruption, sequence errors, and
   invalid tails.
4. Ship `tracy-stream inspect` and explicit `recover --truncate`.

Exit gate: all unit/CTest cases pass in Release without elevation and a damaged
tail never exposes a partially committed record.

Status: **complete**.

### Stage 2 — compatibility recorder

1. Add a protocol observer to the existing capture `Worker`.
2. Record exact incoming handshake/frame bytes and every outgoing status/query
   packet in causal order.
3. Use bounded disk buffers and pause socket reads while a buffer is pending.
4. Append terminal status/counters and a durable session end.

This stage still uses the existing `Worker` for protocol resolution and may
retain timeline data in memory. It provides a correct transcript and recovery
oracle before timeline allocation is removed.

Exit gate: a live TPS capture grows continuously on disk, survives forced
termination at every injected boundary, and leaves no silent disk errors.

Status: **complete**, including real Godot/TPS capture.

### Stage 3 — replay and `.tracy` compatibility

1. Implement a transcript transport that replays client-to-server records and
   validates the server-query sequence.
2. Feed replay into `Worker`, then use the established snapshot writer to
   produce a normal `.tracy`.
3. Add deterministic replay diagnostics for protocol/query divergence.
4. Compare MCP results between the direct `.tracy` control and replayed output.

Exit gate: overview, search, plots, timeline, validation, comparison, and
available frame-image queries agree for classified fixtures.

Status: **complete for synthetic protocol capture and classified existing TPS
fixtures**.

### Stage 4 — bounded protocol recorder

1. Extract the minimum protocol-definition resolver from `Worker`.
2. Remove timeline/event retention from the recording process.
3. Enforce configured memory and disk queue limits with measurable TCP
   backpressure.
4. Add throughput, slow-disk, disk-full, disconnect, and long-duration soak
   tests.

Exit gate: resident memory is bounded independently of capture duration and no
committed wire record is lost under supported termination cases.

Status: **complete**. The default hard bounds are 512 MiB, 8,000,000 retained
definitions/state entries, and 2,000,000 queued definition queries. Slow-sink
backpressure and injected write/flush failures are covered by Release tests.
ProtocolOnly does not retain the active-allocation set; memory events remain in
the journal while only their names, threads, callstack definitions, and query
state contribute to recorder memory.

### Stage 5 — live query path

1. Add an append-only segment store over committed journal records.
2. Publish immutable `TraceReadView` revisions with a monotonic watermark.
3. Implement `SegmentTraceSource`, session revision updates, and capability
   reporting.
4. Exercise the existing MCP surface against fixed live revisions; define
   partial-result behavior beyond the watermark.

Exit gate: identical requests against a fixed revision are stable, readers never
observe uncommitted bytes, and post-capture results match snapshot MCP results.

Status: **complete**. `SegmentTraceSource` serves the full existing query/MCP
surface from immutable committed revisions and preserves the last good source
when a refresh is partial or fails.

## Automated acceptance matrix

| Layer | Automated now | Deferred only when elevation is required |
| --- | --- | --- |
| Journal | Unit, corruption, truncation, resume, CLI | None |
| Protocol | Synthetic client/server transcript and replay | None |
| Existing MCP | Contract tests, stdio transcript, classified local traces | None |
| Godot/TPS | Real D3D12 TPS ProtocolOnly capture, strict replay, direct query | None required for this milestone |
| End to end | Stream growth, local drain completion, forced termination, replay, MCP A/B | None |

Fixtures are classified before use: corrupt captures must fail explicitly,
captures without frame images skip that capability, and full captures exercise
the complete MCP matrix. Validation timeouts scale with trace size rather than
using the previous fixed 180-second ceiling.
