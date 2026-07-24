# tracy-stream

`tracy-stream` is the streaming-capture layer for Tracy Structured Data.
It provides a portable `.tracy-stream` append journal with
CRC32C records, explicit commit trailers, durable boundaries, recovery scanning,
and opt-in tail repair. `tracy-capture` writes the journal continuously through
an asynchronous strict two-buffer relay. Journal-only capture uses a bounded
`Worker::Mode::ProtocolOnly` resolver instead of retaining the timeline.
`tracy-stream-convert` strictly replays a valid journal prefix into a standard
`.tracy` snapshot.

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

Either output may be omitted. With only `-j`, the recorder defaults to a
512 MiB memory limit, 8,000,000 retained definitions/state entries, and 2,000,000
queued definition queries. Exceeding a limit or encountering a write/flush
failure terminates explicitly; records are never silently dropped. A timed
local stop records a replay-external `BeginDrain` boundary, processes only
responses to already-issued definition queries, then sends the ordinary
`ServerQueryTerminate` packet and writes the terminal record.

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

Query a committed live or completed journal directly:

```powershell
query/build/Release/tracy-query.exe `
  --trace output.tracy-stream --allow-root (Split-Path output.tracy-stream)
```

`SegmentTraceSource` strictly replays each immutable committed revision, then
publishes it atomically. Requests already using an older revision keep that
view; a partial or invalid refresh cannot replace the last good revision.

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
records the backpressure, LIVE-read, and reliability behavior used in the
design. [Acceptance results](docs/acceptance-results.md) include the real
Godot/TPS ProtocolOnly run, and the [repository state report](docs/repository-state.md)
records the final single-worktree layout.
