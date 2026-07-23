# Acceptance and performance verification

The normal CTest suite contains deterministic statistics/Memory/GTMEM1 tests, a full-domain `FakeTraceSource`, query contract tests, MCP transcripts, CLI version/doctor checks, and, when local captures are configured, Worker and ChatGPT-style end-to-end tests.

Set the local capture root before configuring:

```powershell
$env:TRACY_ACCEPTANCE_TRACE_ROOT = 'C:\CodeProjects\GodotProjects'
cmake -S query -B C:\CodeProjects\GodotProjects\tracy-query-build -G 'Visual Studio 18 2026' -A x64 -DGIT_REV=HEAD
cmake --build C:\CodeProjects\GodotProjects\tracy-query-build --config Release --parallel
ctest --test-dir C:\CodeProjects\GodotProjects\tracy-query-build -C Release --output-on-failure
```

The committed fixture path is optional; large local traces are never added to Git. The end-to-end test opens Fresh1/Fresh2, validates the first trace, exercises source resources, submits and retrieves an asynchronous analysis job, and compares frames, zones, and embedded source. It also runs the same three comparison modes for Connect1/Connect2 and Reconnect-1/Reconnect-2. The Startup capture is retained as a corrupt-input regression: it must fail quickly with `CORRUPT_TRACE`, not spin while reading a declared record count beyond EOF.

The measurements from the implementation acceptance run are recorded in [acceptance-results.md](acceptance-results.md).

Manual acceptance additionally uses Fresh/Connect/Reconnect/Startup/GPU/root captures. Verify Memory snapshot start/allocated/freed/end counts and bytes against the GUI, GTMEM1 pass pairing and use masks, reconnect-safe pool/allocation IDs, absent-domain capabilities, resource bounding, and three randomly selected evidence refs.

Performance gates:

- MCP initializes with no trace within the 10-second startup budget.
- `trace.open` returns its queued/loading handle within one second.
- Ready `trace.status`/`system.describe` target 200 ms; warm large-trace overview targets two seconds.
- A session never reloads its `.tracy` for later queries.
- Ordinary responses stay below 8 MiB; cache usage stays under its configured budget.
- Profiler load time and peak RSS should remain within 10% of the recorded `a7813c11` baseline. A platform-dependent breach must be investigated and documented rather than waived silently.

CI builds and tests query independently on Windows, Linux, and macOS. Emscripten does not build query. Release verification also rebuilds profiler, capture, and csvexport and confirms the original packaged profiler hash is unchanged.
