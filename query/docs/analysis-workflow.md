# Evidence-driven analysis workflow

Start with `tracy_trace_open`; it returns immediately. Poll `tracy_trace_status` until the snapshot is `ready`. Then call `tracy_overview` and `tracy_validate` before forming a performance hypothesis.

For a single trace:

1. Check frame statistics and outliers.
2. Compare CPU inclusive/self/running hotspots and GPU hotspots.
3. Inspect the relevant frame/time range and parent-child zone tree.
4. Check active memory, frame snapshot/diff, leak candidates, and D3D12/GTMEM1 attribution.
5. Inspect lock wait distributions and context-switch wake/run/migration evidence.
6. Resolve callstacks, symbols, disassembly, or bounded source only where it can confirm a hypothesis.
7. Cross-check each conclusion through a second query path.

For two traces, open both sessions and use `tracy_compare` with `kind=frames`, `zones`, and `source`. Zone matching uses normalized source location/name; frame sets use normalized names; source comparison returns bounded unified hunks from embedded caches.

An evidence citation should include the trace fingerprint plus the most specific refs available: frame, CPU/GPU zone, memory event/allocation, GTMEM1 pass, callstack, symbol, and source location. A useful final statement distinguishes fact from inference—for example, “frame 412 is 8.3 ms slower and RenderGraph self time accounts for 6.9 ms” before proposing why.

Example questions:

```text
Analyze Test-root.tracy. Find the slowest frames, CPU/GPU hotspots, memory growth,
lock contention, and scheduling anomalies. Cross-check every conclusion and cite refs.
```

```text
Compare Fresh1 and Fresh2 by frame and zone distributions, then check whether bounded
embedded-source differences explain the regression.
```

```text
For frame 120, list D3D12 allocations active at start, allocated/freed in the frame,
and active at end. Explain their GTMEM1 request scope and pass uses, including ambiguity.
```
