# Streaming acceptance results

- Date: 2026-07-24
- Branch: `feature-StructuredData`
- Pre-delivery HEAD: `c2dc6c4721396e80308b16c12f00d53518d1ae01`

## Release tests

- `stream` Release CTest: 2/2 passed.
- `query` Release project tests: 4/4 passed (`contract`, MCP transcript,
  version, and doctor).
- CRC32C, every-byte truncation, corrupt header/payload/trailer/commit,
  sequence errors, resume, explicit repair, and immutable revision checks pass.
- Short writes are retried. Zero-progress, write, publish-flush, and
  durable-flush failures poison the sink and propagate to capture.
- The slow-sink test confirms the strict two-buffer relay blocks producers,
  preserves order, drops no records, and reports backpressure/peak bytes.
- `LocalControl` is stored as `Diagnostic`, excluded from replayable server
  byte counts, and retained through the terminal record.

## Synthetic end-to-end acceptance

Artifact:

```text
C:\Users\Admin\Documents\JN-Unity-T3\tracy-final-build\acceptance-final\
stream-acceptance-20260724-060744-397
```

Results:

- 142 committed records and 18,190 journal bytes;
- 28 immutable revisions observed while the capture was live;
- monotonic watermark and committed-prefix bounds held;
- strict replay validated 126 client and 12 server records;
- direct/replayed counts, last time, and frame statistics matched;
- MCP A/B passed all 12 tools, resources, progress/cancellation, validation,
  comparisons, search, plots, and timeline;
- forced termination left an incomplete but recoverable 20-record prefix,
  which converted and passed query doctor.

A separate clean `ProtocolOnly` fixture validated the local drain-control
path:

```text
C:\Users\Admin\Documents\JN-Unity-T3\tracy-final-build\
protocol-only-graceful\20260724-051552-034.tracy-stream
```

- 143 committed records, including one `LocalControl` at sequence 141;
- the ordinary `ServerQueryTerminate` and `SessionEnd` follow the marker;
- strict replay: 125 client records versus 13 server records;
- direct `.tracy-stream` doctor and overview passed;
- query metadata reported `source_kind=segment`, `revision=143`,
  `complete=true`.

Persistent live-query polling also observed revision growth `29 -> 106 -> 385`
and completion `false -> false -> true`; older immutable sources remained
usable across swaps.

## Real Godot/TPS D3D12 acceptance

Input repositories:

```text
Godot:   C:\CodeProjects\GodotProjects\godot
TPS:     C:\CodeProjects\GodotProjects\tps-demo
Journal: C:\CodeProjects\GodotProjects\tracy-captures\
         TPS-protocol-only-20260724-054750.tracy-stream
```

The 3-second high-detail run enabled full D3D12 memory tracking, resource
passes, frame images, and the TPS producer's normal CPU/GPU callstacks. The
per-memory-event extra callstack option was disabled to keep this acceptance
bounded. The journal finalized after a 176.5-second response-only definition
drain; a local-control marker is present at sequence 10,912 and the terminal
record is sequence 29,803.

- journal size: 189,311,601 bytes;
- protocol events observed by the bounded recorder: 37,730,444;
- retained definitions/state: 3,619,879;
- committed records/revision: 29,803;
- client bytes: 145,713,512;
- replayable server bytes: 41,209,687;
- strict replay validated 11,798 client records against 17,834 server records;
- replayed `.tracy` file: 83,049,174 bytes;
- direct `.tracy-stream` doctor and overview passed in the query process;
- result source: `segment`, revision 29,803, complete;
- reconstructed trace includes 3,205 CPU zones, 112 frames, 3 frame images,
  321,857 callstack payloads, 698,135 memory events, 5,130 locks,
  1,306 plot points, 2,925,163 callstack frames, and 226,392 symbols;
- GPU-memory, memory, frame-image, lock, callstack, symbol, source, plot,
  message, frame, CPU-zone, and timeline domains are queryable.

`tracy-stream inspect` reports `complete=true` and the full file as the valid
prefix. The strict converter reproduced the server stream byte for byte before
writing the snapshot. The administrator harness's 180-second process deadline
landed immediately after the terminal output on this high-definition workload;
the committed artifact itself was already finalized and durable. Synthetic
acceptance separately covers prompt normal process exit.

Earlier runs exposed the final-query race and a
`QueueLockRelease` field-decoding error. Both now fail regression checks on the
old fixtures and are fixed by the bounded resolver plus local drain protocol.

## Limits and interpretation

The recorder is duration-independent for timeline/event retention, but the
number of unique protocol definitions is workload-dependent and deliberately
hard-bounded. Defaults are 512 MiB, 8,000,000 definition/state entries, and
2,000,000 queued queries. Hitting any limit is an explicit recorder failure, not
data loss hidden behind a nominally complete journal.

ProtocolOnly does not retain an active-allocation address set. Memory event
bytes remain exact in the journal, but only definition/query state is retained
in the recorder. On symbol-heavy instrumented applications, resolving optional
symbol/source definitions can dominate graceful drain time; this affects
completion latency, not continuous journal durability or its recovery prefix.

The current direct-query implementation strictly replays a committed revision
into a temporary snapshot. It provides full MCP compatibility and immutable
revision semantics; incremental native segment indexes remain a performance
optimization for a later milestone.
