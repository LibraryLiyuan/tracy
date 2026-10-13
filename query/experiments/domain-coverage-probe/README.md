# Domain coverage contract and memory audit

This isolated native experiment does two things:

1. Tests a reference candidate **union** policy and recorded memory sample
   handling, including simultaneous CPU/GPU signals and fast-frame VRAM
   exceedance. Input CPU/GPU facts are synthetic and assumed validated.
2. Reads every recorded DXGI Local / Unity Primary Segment usage sample from
   an existing published EventStore. It checks count and peak consistency,
   and reports strict exceedance of both 5 GB and 5 GiB without dropping
   within-frame or same-timestamp peaks.

It does **not** fix or validate v3 CPU/GPU fact extraction, turn GPU time-window
overlap into frame causality, reconstruct unrecorded peaks, or implement all of
production v4. `all_domain_coverage_complete` remains false. Primary Segment
semantics require adapter/UMA validation; counters may publish cached values.

## Build and test

Uses matching existing Release store libraries, as the frame-shortlist probe
does. Source root, library path and JSON include can be overridden in CMake.

```powershell
cmake -S E:\tracy\query\experiments\domain-coverage-probe -B E:\tracy\_build-all\domain-coverage-probe -G 'Visual Studio 17 2022' -A x64 -DTRACY_JSON_INCLUDE=E:/tracy/_build-all/_cpm/json/b88ca108e0b5a597e859a21658d11fa5f1feb410/include
cmake --build E:\tracy\_build-all\domain-coverage-probe --config Release --parallel 4
ctest --test-dir E:\tracy\_build-all\domain-coverage-probe -C Release --output-on-failure
```

Run with a **new** output directory:

```powershell
& E:\tracy\_build-all\domain-coverage-probe\Release\tracy-domain-coverage-probe.exe F:\JNTracyCapture\Player_20261011_001534\Player_20261011_001534.tracy-stream F:\JNTracyCapture\Player_20261011_001534\analysis-60fps-20261011\coverage-audit-20261011\manual-new-1
```

Requires the published schema-17 store; no snapshot conversion or indexing
fallback. Uses recorded file size/time/ID verification, not a fresh full-file
hash. Plot/sample reads use bounded pages with cursor progress checks, a
one-million-sample-per-stream guard, and require complete count agreement.
No Python trace processing is used.

Read the scope, outstanding production work and full coverage contract in:

`E:/tracy/doc/Tracy-Analyze-v4-Domain-Coverage-Audit-20261011.zh-CN.md`
