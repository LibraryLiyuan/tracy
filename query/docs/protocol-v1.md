# tracy-query/1 protocol

## Envelope

Every CLI/NDJSON request is an object with `protocol`, `id`, `method`, and optional `params`:

```json
{"protocol":"tracy-query/1","id":"request-1","method":"frame.outliers","params":{"trace_id":"trace-1","limit":20}}
```

A success contains `schema_version=1.5.0`, `ok=true`, `data`, warnings, and—when relevant—a trace read view and page. A failure contains `ok=false` plus a stable error code, human-readable message, retryability, and structured details. The authoritative envelope schema is `schema/tracy-query-v1.schema.json`; one canonical operation registry generates `system.describe`, `system.schema`, and the generic MCP inspect schema. Schema 1.2 added typed Capture Context and Producer Quality queries. Schema 1.3 added versioned `JNCAT1` stable definitions and generation-bearing `JNENT1` entities. Schema 1.4 added exact FrameIdentity and typed cross-domain correlation queries. Schema 1.5 adds bounded scan/CPU/node/group budgets, explicit partial-result metadata, raw-position cursors, and stream-completeness diagnostics.

## Numeric and range rules

- Nanosecond timestamps/durations, byte counts, addresses, and native thread IDs are strings, preventing JavaScript 64-bit precision loss.
- Addresses use `0x...`; other integers use decimal strings.
- Line numbers, list indices, limits, and returned counts are JSON numbers.
- Means, ratios, standard deviation, and percentiles are JSON numbers.
- Time ranges are half-open: `[start_ns,end_ns)`.
- An unfinished event has `end_ns=null` and `complete=false`.
- Trace strings and source/code text carry an untrusted-data marker.

## Stable refs and cursors

Entities expose trace-scoped opaque refs such as `tracy:v1:<fingerprint-prefix>:cpu-zone:<id>`. Clients must not parse them. A ref from another trace is rejected.

List methods default to 100 entries and accept at most 1000. Top-N analyses default to 20 and accept at most 500. Cursors are opaque Base64URL values bound to the session, revision, method, normalized filters, sorting, logical offset, and raw scan position. Budget values are intentionally excluded from cursor binding so a caller may resume a partial page with a larger budget. Reusing a cursor after changing semantic parameters or closing the trace returns `STALE_CURSOR`.

## Budgets and partial results

Every request accepts `max_scan_events`, `max_cpu_ms`, `max_nodes`, and
`max_groups`. The defaults are 5,000,000 events, 5,000 ms, 10,000 nodes, and
500 groups; the hard maxima are 100,000,000 events, 60,000 ms, 100,000 nodes,
and 10,000 groups.

Every successful envelope contains `partial`, `omitted_count`, and `budget`.
When no budget is exhausted, `partial=false`, `omitted_count="0"`, and the
count is exact. When work stops at a budget, `partial=true`; an unknown omitted
count is `null`, never a fabricated zero. Paginated scans also return a
`next_cursor` at the first unconsumed raw record, so resuming produces neither
duplicates nor omissions. Cancellation is different from a partial result: a
cancelled request returns the stable `CANCELLED` failure code.

While a trace is opening, `trace.status` also returns `load_progress` with the native Worker stage plus its global and per-stage counters. Loading is serialized, so these counters belong to the sole active loader. After native loading completes, the stage changes to `analysis_indexes` before the session becomes ready.

Deterministic ordering always includes an entity ref or stable index as its final tie-breaker. Source and code results default to 64 KiB and are limited to 1 MiB. Ordinary responses are limited to 8 MiB, requests to 1 MiB, callstacks to 256 frames, and decoded frame images to 16 MiB and 4096×4096.

## Filtering

No SQL, regular expression, or arbitrary expression evaluator is exposed. String filtering supports:

```json
{"filter":{"mode":"contains","value":"Render","case_sensitive":false}}
```

Modes are `exact`, `contains`, and `prefix`. `fields` performs bounded response projection while retaining `ref`.

## Domains

`system.capabilities` reports `present`, `queryable`, `indexed`, `reason`, methods, and limits for every domain. The complete method-to-Worker-data map is in `schema/coverage-v1.json`. The main groups are trace/session, threads and scheduling, frames/images, CPU/GPU zones, callstacks/samples/symbols/source, memory/GTMEM1, locks, plots/messages, statistics, compare, and validation.

## Capture context and producer quality

Schema 1.2 reads versioned `JNCTX1` Capture Context and `JNQ1` Producer Quality
envelopes from trace AppInfo. These records are untrusted trace data. A valid
result has exactly one `connection_id`, and that ID must match the connection
record in Capture Identity. Records from different connections are never
merged into a complete result.

- `capture.context` merges process-monotonic context generations and reports
  build identity, runtime, workload, and capture configuration layers.
- `capture.coverage` and `producer.list` return exact closed-window counter
  deltas and distinguish covered, filtered, degraded, real-zero, disabled,
  unsupported, permission-denied, deferred, and unknown states.
- `producer.get` requires `trace_id` and stable producer `key`; it does not
  accept or require an entity `ref`.

Snapshot, committed `.tracy-stream`, and stream-replayed `.tracy` inputs use
the same result contract. A trace that predates these envelopes returns
`present=false` instead of fabricating empty context or producer data.

## Stable catalog and capture-local entities

Schema 1.3 reads `JNCAT1` and `JNENT1` AppInfo envelopes. `catalog.kinds`,
`catalog.list`, `catalog.get`, `catalog.entities`, and `catalog.quality` expose
versioned DefinitionKeys, normalized SourceFileIds, generation-bearing
EntityIds, unresolved references, collisions, resource limits, and privacy
violations. DefinitionKeys are stable across captures; catalog IDs and
EntityIds are not. An old trace reports the catalog capability as absent.
On-demand Tracy clients retain incremental AppInfo across reconnects. Catalog
queries use Capture Identity's current connection ID and report older catalog
envelopes as `records.stale_connection`; stale EntityIds are never surfaced in
the current capture-local entity namespace.

## Errors

The fixed set is:

```text
INVALID_REQUEST METHOD_NOT_FOUND INVALID_PARAMS PATH_NOT_ALLOWED
TRACE_NOT_FOUND TRACE_OPEN_FAILED UNSUPPORTED_TRACE_VERSION CORRUPT_TRACE
TRACE_LOADING TRACE_NOT_READY TRACE_CLOSED CAPABILITY_UNAVAILABLE
ENTITY_NOT_FOUND INDEX_NOT_READY STALE_CURSOR RESOURCE_LIMIT
CANCELLED INCOMPLETE_DATA INTERNAL_ERROR
```

Batch and MCP modes return business errors without terminating the process. Single-request mode returns a nonzero exit code for failures.

## Compatibility

`WorkerTraceSource` accepts the trace versions supported by Tracy 0.13.1 (0.9.0 through 0.13.1). A newer capture returns `UNSUPPORTED_TRACE_VERSION`; an older unsupported capture returns an explicit legacy-version message under the same stable code. Missing fields in older captures remain unavailable—they are never fabricated.
