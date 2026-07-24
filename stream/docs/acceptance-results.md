# Streaming acceptance results

- Date: 2026-07-24
- Branch: `feature-StructuredData`
- Pre-delivery HEAD: `09bc03e1`

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
stream-acceptance-20260724-115307-670
```

Results:

- 152 committed records and 19,439 journal bytes;
- 29 immutable revisions observed while the capture was live;
- monotonic watermark and committed-prefix bounds held;
- strict replay validated 136 client and 12 server records;
- direct/replayed counts, last time, and frame statistics matched;
- MCP A/B passed all 12 tools, resources, progress/cancellation, validation,
  comparisons, search, plots, and timeline;
- the dedicated ProtocolOnly run completed while its producer was still
  running, proving that the native disconnect stops the capture boundary;
- its SessionBegin v2 payload declared deferred symbol expansion before any
  drain control record existed;
- its v4 `LocalControl` payload was five bytes, contained a query window in
  `[1, 8192]`, and was immediately followed by recorded
  `ServerQueryDisconnect`;
- the drain fixture had 150 records and only 6 client bytes after the marker;
- forced termination left an incomplete but recoverable 19-record prefix,
  which converted and passed query doctor.

## Real Godot/TPS D3D12 acceptance

Input repositories:

```text
Godot:   C:\CodeProjects\GodotProjects\godot
TPS:     C:\CodeProjects\GodotProjects\tps-demo
```

The final test build enabled full D3D12 memory tracking, resource passes, frame
images, on-demand recording, and 62-level CPU callstacks. Godot was rebuilt
against the modified Tracy Client before the final runs.

### Runaway regression evidence

The original nominal five-second capture did not terminate. The user stopped
it after it reached 17,063,103,979 bytes (15.89 GiB):

```text
C:\CodeProjects\GodotProjects\tracy-captures\manual-stream\
TPS-stream-smoke-20260724-174148.tracy-stream
```

The drain marker was around 70.3 MiB. After it, the recorder accepted 505,346
records, including about 15.42 GiB of client payload. The old server-only drain
therefore kept recording ordinary events from the still-running game instead
of converging on a fixed tail.

A later native-disconnect run exposed a second race: on-demand Client
`HandleDisconnect()` cleared normal queues while Symbol Worker asynchronously
placed callstack responses in those queues. One run ended by luck; another
stalled after the last batch of 117 callstack queries. The final implementation
stops normal producers but drains the pre-disconnect and definition-response
queues instead of clearing them.

### Final v4 control and SessionBegin v2 runs

Two consecutive five-second high-detail captures completed normally:

| Journal | Capture | Drain | Size | Records | Post-marker client bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| `TPS-protocol-only-20260724-105424.tracy-stream` | 5.05 s | 2.32 s | 75,390,903 | 15,209 | 757,074 |
| `TPS-protocol-only-20260724-105536.tracy-stream` | 5.03 s | 2.88 s | 83,241,470 | 10,576 | 939,616 |

Both v4 control records captured the live server-query window of 5,037 and
recorded `ServerQueryDisconnect` as the first post-marker server query. Both
ended with `ProtocolCloseReason::CaptureComplete`, passed `tracy-stream
inspect`, strict byte-validated conversion, and query doctor.

Strict replay results:

- first run: 3,941 client records and 11,257 server records; output
  29,423,704 bytes;
- second run: 4,343 client records and 6,221 server records; output
  31,926,684 bytes.

The final writer additionally emits SessionBegin v2 with the
`DeferredSymbolExpansion` session flag. A ten-second real run validated both
live and complete revisions:

```text
C:\CodeProjects\GodotProjects\tracy-captures\
TPS-live-v2-20260724-112410.tracy-stream
```

- capture: 10.06 seconds; drain: 8.02 seconds;
- journal: 79,516,492 bytes and 13,305 committed records;
- live query doctor passed while capture was still active and the journal was
  11,973,843 bytes, before any drain marker existed;
- v4 marker at sequence 12,426, query window 5,037, with 1,365,769 client
  payload bytes after the marker;
- strict replay validated 4,400 client and 8,882 server records and produced a
  30,726,331-byte `.tracy`;
- direct journal and converted snapshot both passed doctor and all 12 MCP tools,
  including three compare kinds, asynchronous validation, search, plots,
  timeline, frame-image resources, and source resources;
- semantic overview: 893 frames, 66,196 CPU zones, 508,957 callstack payloads,
  86,684 callstack frames, 1,694,242 memory events, 41,346 locks, 29 frame
  images, and 36,038 plot points;
- the journal reported `source_kind=segment`, revision 13,305, complete; the
  converted file reported `source_kind=snapshot`.

This run also exposed a replay-only completion mismatch. Full Worker had
validated every recorded query but retained a pending state that ProtocolOnly
does not use, so it omitted the final `ServerQueryTerminate`. ProtocolOnly
journal replay now retains Full timeline data while using the recorder drain
completion predicate. The same test then matched all 8,882 server records.
Server-record verification also uses a real ten-second deadline and closes the
local replay socket on failure, preventing a malformed or divergent journal
from hanging converter or Query indefinitely.

Optional instruction-level symbol expansion is intentionally deferred in
ProtocolOnly v4, so this capture has function/file/line callstack frames and
source data but no persisted symbol-code/disassembly domain. This is a bounded
drain tradeoff, not loss caused by conversion: direct stream and converted
snapshot report the same domain availability and counts.

Small legacy fixtures with drain control versions 1, 2, and 3 also converted
and passed query doctor. Version 4 is required for newly recorded detailed
captures because it stores the original query window and therefore preserves
deterministic priority-query ordering across different socket buffer sizes.

## Limits and interpretation

The recorder is duration-independent for timeline/event retention, but the
number of unique protocol definitions is workload-dependent and deliberately
hard-bounded. Defaults are 512 MiB, 8,000,000 definition/state entries, and
2,000,000 queued queries. Hitting any limit is an explicit recorder failure, not
data loss hidden behind a nominally complete journal.

ProtocolOnly does not retain an active-allocation address set. Memory event
bytes remain exact in the journal, but only definition/query state is retained
in the recorder. Version 4 no longer recursively expands optional symbol code
and disassembly addresses, but a large pre-disconnect queue or required dynamic
definitions can still lengthen graceful drain. A second Ctrl+C force-stops an
abnormal drain with exit code 130 and deliberately leaves an incomplete,
recoverable prefix instead of a false complete session.

The current direct-query implementation strictly replays a committed revision
into a temporary snapshot. It provides full MCP compatibility and immutable
revision semantics; incremental native segment indexes remain a performance
optimization for a later milestone.
