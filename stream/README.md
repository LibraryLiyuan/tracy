# tracy-stream

`tracy-stream` is the streaming-capture work area for Tracy Structured Data.
The first delivered layer is a portable `.tracy-stream` append journal with
CRC32C records, explicit commit trailers, durable boundaries, recovery scanning,
and opt-in tail repair. `tracy-capture` can write the journal continuously while
it performs normal Tracy protocol resolution, and `tracy-stream-convert`
replays a valid journal prefix into a standard `.tracy` snapshot.

`JournalStore` publishes immutable read views over only the committed prefix.
Its revision is the last committed record sequence; its watermark is the last
committed monotonic timestamp. A partial live tail does not change either
value. Each published prefix also carries a CRC32C fingerprint, so a
structurally valid in-place rewrite, truncate, or replacement never replaces
the last good view.

Build and test it independently:

```powershell
cmake -S stream -B stream/build -DBUILD_TESTING=ON
cmake --build stream/build --config Release
ctest --test-dir stream/build -C Release --output-on-failure
```

Inspect without changing a journal:

```powershell
stream/build/Release/tracy-stream.exe inspect capture.tracy-stream --records 20
```

Tail repair is intentionally explicit:

```powershell
stream/build/Release/tracy-stream.exe recover capture.tracy-stream --truncate
```

Record a journal alongside a normal snapshot:

```powershell
capture/build/Release/tracy-capture.exe `
  -j output.tracy-stream -o output.tracy -s 30
```

Either output may be omitted. The current compatibility recorder still uses
`Worker` internally, so journal-only mode does not yet provide duration-
independent process memory.

Automation that cannot add `-j` may set the process-local
`TRACY_STREAM_OUTPUT` environment variable to the journal path. An explicit
`-j` argument takes precedence.

Convert a complete or recoverable-prefix journal:

```powershell
capture/build/Release/tracy-stream-convert.exe `
  -i output.tracy-stream -o replayed.tracy
```

The converter replays client bytes through `Worker` and validates the recorded
server handshake/query byte stream before saving the snapshot.

Run the unattended producer/capture/replay/MCP A/B acceptance workflow:

```powershell
stream/tests/ProtocolCaptureAcceptance.ps1 `
  -ProducerExe stream/build/Release/tracy-stream-test-producer.exe `
  -CaptureExe capture/build/Release/tracy-capture.exe `
  -ConverterExe capture/build/Release/tracy-stream-convert.exe `
  -InspectorExe stream/build/Release/tracy-stream.exe `
  -QueryExe path/to/tracy-query.exe `
  -OutputRoot stream/build
```

See [the format contract](docs/format-v1.md) and
[the staged implementation plan](docs/implementation-plan.md). The
[Unreal Insights source audit](docs/unreal-insights-streaming-analysis.md)
records the exact backpressure, LIVE read, and reliability behavior used in the
design, and [acceptance results](docs/acceptance-results.md) separate automated
non-administrator coverage from the deferred elevated TPS matrix. The
[repository state report](docs/repository-state.md) records the branch and
worktree isolation used for this work.
