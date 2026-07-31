# tracy-query/1 protocol

## Envelope

Every CLI/NDJSON request is an object with `protocol`, `id`, `method`, and optional `params`:

```json
{"protocol":"tracy-query/1","id":"request-1","method":"frame.outliers","params":{"trace_id":"trace-1","limit":20}}
```

A success contains `schema_version=1.1.0`, `ok=true`, `data`, warnings, and—when relevant—a trace read view and page. A failure contains `ok=false` plus a stable error code, human-readable message, retryability, and structured details. The authoritative envelope schema is `schema/tracy-query-v1.schema.json`; `system.describe` supplies the method inventory, required arguments, examples, numeric rules, and active limits. Schema 1.1 adds the typed `trace.identity` method without changing the Tracy live protocol or trace file version.

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

List methods default to 100 entries and accept at most 1000. Top-N analyses default to 20 and accept at most 500. Cursors are opaque Base64URL values bound to the session, revision, method, normalized parameters, sorting, and offset. Reusing a cursor after changing parameters or closing the trace returns `STALE_CURSOR`.

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
