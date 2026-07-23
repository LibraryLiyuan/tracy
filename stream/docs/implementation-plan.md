# Tracy streaming implementation and acceptance plan

## Baseline

The implementation branch is `feature-stream` in an isolated worktree and is
based on `feature-StructuredData` (`75cce700`). The existing MCP/query worktree,
Godot producer repository, and TPS demo repository remain independent.

The current Tracy capture path constructs a full `Worker`, retains the capture
in memory, and writes a normal `.tracy` snapshot only after disconnect. Tracy's
wire protocol is bidirectional, so a byte-only relay cannot replace the server:
the server must continue issuing definition and frame-image queries.

Unreal Insights provides a useful backpressure model: socket reads are gated by
completion of bounded asynchronous file buffers, and live readers tolerate
temporary EOF. It does not provide an application ACK/retransmit or
power-failure commit format, so `.tracy-stream` adds explicit record integrity
and commit boundaries.

## Execution snapshot

Completed in the current implementation:

- isolated `feature-stream` worktree and repository-state audit;
- v1 file/record/trailer contract;
- writer, scanner, explicit repair, resume, fault injection, and CLI;
- exact bidirectional `Worker` protocol observer with durable checkpoints;
- offline replay with outgoing query-byte validation and `.tracy` conversion;
- immutable `JournalStore` read views with revision/watermark publication and
  committed-prefix fingerprint revalidation;
- live growth polling plus direct/replayed MCP A/B acceptance;
- source-level Unreal Engine 5.7.2 Recorder/Store/TraceAnalysis audit.

The compatibility recorder intentionally remains the Stage 2 implementation:
it performs bounded synchronous write-ahead recording but still retains the
existing `Worker` timeline. Stage 4 requires extracting Tracy's definition and
server-query resolver; Unreal's one-way relay cannot replace it. Stage 5's
committed-prefix Store is implemented, while a domain-queryable
`SegmentTraceSource` remains gated on the self-describing event/definition
model produced by Stage 4. These limitations are explicit rather than hidden
behind a nominal "stream" mode.

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

Status: **implementation complete; synthetic live acceptance complete; elevated
TPS process matrix deferred until the user returns**.

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

Status: **not yet complete**. This is the next recorder architecture milestone,
not an administrator-test blocker.

### Stage 5 — live query path

1. Add an append-only segment store over committed journal records.
2. Publish immutable `TraceReadView` revisions with a monotonic watermark.
3. Implement `SegmentTraceSource`, session revision updates, and capability
   reporting.
4. Exercise the existing MCP surface against fixed live revisions; define
   partial-result behavior beyond the watermark.

Exit gate: identical requests against a fixed revision are stable, readers never
observe uncommitted bytes, and post-capture results match snapshot MCP results.

Status: **Store/revision boundary complete; domain `SegmentTraceSource`
pending Stage 4's event/definition model**.

## Automated acceptance matrix

| Layer | Automated now | Deferred only when elevation is required |
| --- | --- | --- |
| Journal | Unit, corruption, truncation, resume, CLI | None |
| Protocol | Synthetic client/server transcript and replay | None |
| Existing MCP | Contract tests, stdio transcript, classified local traces | None |
| Godot/TPS | Plan validation, executable/manifest checks, non-elevated runs | D3D12 capture runner that requires Administrator |
| End to end | Stream growth, forced process termination, replay, MCP A/B | Final administrator-only GPU capture matrix |

Fixtures are classified before use: corrupt captures must fail explicitly,
captures without frame images skip that capability, and full captures exercise
the complete MCP matrix. Validation timeouts scale with trace size rather than
using the previous fixed 180-second ceiling.
