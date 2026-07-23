# Tracy Query v1

`tracy-query` is the read-only, headless structured-query companion to the Tracy profiler. It opens a completed `.tracy` capture with `FileRead + Worker(EventType::All)`, waits for Tracy's background indexing, and exposes bounded JSON/NDJSON and MCP STDIO queries. It does not use ImGui, call an LLM, open a network connection, create SQLite, or write a sidecar.

The analysis core is shared with `tracy-profiler`: frame-accurate memory snapshots and D3D12/GTMEM1 pass attribution are calculated by `TracyAnalysis`, while `TracyView_Memory.cpp` retains only GUI selection and drawing state. `QueryService` depends on the storage-neutral `TraceSource` contract; only `WorkerTraceSource` includes `TracyWorker.hpp`, so a future `SegmentTraceSource` can implement the same batch interface.

## Build

```powershell
cmake -S query -B ..\tracy-query-build -G "Visual Studio 18 2026" -A x64 -DGIT_REV=HEAD
cmake --build ..\tracy-query-build --config Release --parallel
ctest --test-dir ..\tracy-query-build -C Release --output-on-failure
```

`NO_STATISTICS=ON` is rejected at configure time. Windows, Linux, and macOS are built by the independent `query` CI workflow; Emscripten intentionally does not build this process.

## Executable surface

```text
tracy-query --version
tracy-query --schema
tracy-query --doctor [--trace file.tracy]
tracy-query --trace file.tracy --request request.json|-
tracy-query --trace file.tracy --batch requests.ndjson|-
tracy-query --mcp [--allow-root DIR] [--allow-source-root DIR]
```

`--request` loads once and emits one JSON response. `--batch` loads once and processes NDJSON until EOF. `--mcp` is a UTF-8, one-JSON-object-per-line MCP STDIO server. Protocol data is the only content written to stdout; diagnostics go to stderr.

Trace loading is asynchronous under MCP. At most two sessions are retained, and loads are serialized because Tracy's load progress is global. `tracy_trace_status` reports the native load stage and counters, followed by the `analysis_indexes` stage. Large analyses can be requested with `async=true`; sufficiently large validation, comparison, and analysis calls are promoted automatically to a cooperative job. Poll or cancel them with `tracy_job`.

## Contracts and documentation

- [Protocol v1](docs/protocol-v1.md)
- [Completeness contract](docs/completeness.md)
- [MCP and ChatGPT desktop setup](docs/chatgpt-mcp.md)
- [Security and privacy boundary](docs/security.md)
- [Analysis workflow and examples](docs/analysis-workflow.md)
- [Acceptance and performance tests](docs/acceptance.md)
- [Latest local acceptance results](docs/acceptance-results.md)
- [JSON envelope schema](schema/tracy-query-v1.schema.json)
- [Persisted-domain coverage manifest](schema/coverage-v1.json)
- [Persisted-field coverage ledger](schema/coverage-fields-v1.json)
- [MCP method coverage manifest](schema/coverage-mcp-v1.json)

The implementation supports all persisted data domains enumerated in the domain
manifest. The separate field ledger records every persisted semantic field and
is complete for saved traces accepted by Tracy 0.13.1. Version-dependent fields
carry explicit `field_availability` metadata, and large persisted byte sequences
can be exhausted through bounded chunk reads. MCP completeness is also
`complete`: `QueryMethodRegistry` drives `system.describe`, and every registered
method is reachable through the generic `tracy_inspect` method/params route in
addition to the model-friendly workflows. Producer completeness remains a
separate gate. A missing domain is reported as `present=false`, and a query
against it returns `CAPABILITY_UNAVAILABLE` instead of an ambiguous empty result.
