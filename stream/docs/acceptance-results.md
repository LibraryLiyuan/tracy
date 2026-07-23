# Streaming acceptance results

- Date: 2026-07-23
- Branch: `feature-stream`
- Base: `feature-StructuredData` at `75cce700`

## Automated non-administrator results

### Journal, recovery, and LIVE Store

- Release build succeeded with MSVC 19.51 from both the working build trees and
  brand-new standalone `stream`/`capture` CMake build directories.
- CTest: 2/2 passed.
- CRC32C standard vector passed.
- Every byte truncation point was tested.
- Header, payload, trailer, commit, sequence, and oversize corruption were
  rejected at the first invalid record.
- Short writes were retried; zero-progress, write, and flush failures poisoned
  the writer.
- Resume required explicit tail repair and continued the sequence correctly.
- Tail repair rechecked the open file size and durably flushed truncation.
- Backwards writer/resume timestamps were rejected without consuming sequence
  numbers.
- Concurrent bidirectional observer writes, checkpoints, and durable
  `SessionEnd` passed.
- Immutable store views passed revision, watermark, partial-tail, rollback,
  validly re-encoded prefix mutation, post-terminal mutation, and
  committed-range bounds tests.

### Synthetic real Tracy protocol

Latest unattended acceptance artifact (after the network-close ordering fix):

```text
stream\build-msvc\stream-acceptance-20260723-151140-015
```

Results:

- 142 committed journal records;
- 19,029 journal bytes;
- 28 distinct committed revisions observed while capture was still running;
- live valid size never exceeded observed file size;
- live watermark never moved backwards;
- clean final `SessionEnd`;
- 12/12 rapid connect/capture/disconnect iterations ended with exactly a valid
  complete prefix (25–28 records), exercising terminal/network-thread ordering;
- replay validated 126 client records against 12 recorded server records;
- replayed snapshot was accepted by `tracy-query --doctor`;
- direct/replayed counts, last trace time, and primary frame statistics matched;
- MCP A/B passed all 12 tools, resources, progress/cancellation, validation,
  frame/zone/source comparisons, search, plots, and timeline;
- frame-image checks were capability-skipped because this producer disables
  frame images;
- a second recorder process was forcibly terminated after 20 committed records;
- its missing `SessionEnd` was reported as incomplete, its valid prefix replayed
  into a 1,682-byte `.tracy`, and `tracy-query --doctor` accepted that snapshot.

### Existing query and TPS fixtures

- query contract tests passed;
- MCP transcript tests passed;
- query version and doctor passed;
- MCP Worker integration passed, including compare connect/reconnect and
  corrupt-trace rejection;
- existing TPS baseline/candidate snapshots (40,524,106/44,640,223 bytes)
  passed the full capability-aware MCP workflow again after final hardening:
  all 12 tools, 15 validation checks, frame/zone/source comparisons, resources,
  progress, and cancellation completed in 33 seconds;
- TPS coverage, baseline, and stress runner plans all resolved the intended
  Godot, capture, query, scenario, seed, and output paths without starting a
  process.

## Deferred administrator-only matrix

The TPS runner requires an elevated PowerShell process for the actual Godot
D3D12 capture. These cases are intentionally deferred until the user returns:

1. one coverage warm-up;
2. three baseline runs;
3. three stress runs;
4. repeat with baseline/stress order reversed;
5. inspect every `.tracy-stream`;
6. convert every valid prefix to `.tracy`;
7. run direct/replayed MCP A/B for each pair;
8. force-stop one capture and verify truncated-prefix replay and explicit
   repair;
9. record peak RSS and confirm the current compatibility recorder limitation;
10. after the bounded recorder lands, run slow-disk, disk-full, and soak tests.

The existing TPS runner can be used without modifying the TPS repository.
`TRACY_STREAM_OUTPUT` is inherited by `tracy-capture`, while the runner
continues to pass its ordinary `-o` snapshot:

```powershell
$env:TRACY_STREAM_OUTPUT = 'C:\CodeProjects\GodotProjects\tracy-captures\TPS-stream-coverage-001.tracy-stream'

& 'C:\CodeProjects\GodotProjects\tps-demo\benchmark\run_tracy_capture.ps1' `
  -Scenario coverage `
  -RunId stream-coverage-001 `
  -Seed 424242 `
  -CaptureExe 'C:\Users\Admin\Documents\JN-Unity-T3\tracy-stream-worktree\capture\build-msvc\Release\tracy-capture.exe' `
  -QueryExe 'C:\CodeProjects\GodotProjects\tracy-query-completeness-build\Release\tracy-query.exe' `
  -OutputPath 'C:\CodeProjects\GodotProjects\tracy-captures\TPS-stream-coverage-001.tracy'

Remove-Item Env:TRACY_STREAM_OUTPUT
```

Use unique journal and snapshot names for every run. Classify each fixture
before MCP comparison: corrupt inputs must fail explicitly, and unavailable
domains such as frame images must be capability-skipped rather than treated as
transport failures.
