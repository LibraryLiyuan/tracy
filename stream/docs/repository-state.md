# Repository and worktree state

Final state observed on 2026-07-24 before the streaming delivery commit.

## Tracy

```text
path:   C:\CodeProjects\GodotProjects\tracy-0.13.1
branch: feature-StructuredData
HEAD:   c2dc6c4721396e80308b16c12f00d53518d1ae01
```

`git worktree list --porcelain` reports exactly one worktree:

```text
worktree C:/CodeProjects/GodotProjects/tracy-0.13.1
HEAD c2dc6c4721396e80308b16c12f00d53518d1ae01
branch refs/heads/feature-StructuredData
```

The temporary `tracy-stream-worktree` and the previous structured-data
worktree are no longer registered. Streaming and existing MCP changes are
managed in the original directory through the current branch and commits.

The final streaming changes intentionally touch:

- `server`: protocol observation and bounded `ProtocolOnly` resolution;
- `capture`: journal-only mode and strict conversion;
- `stream`: format, asynchronous journal relay, recovery/store, tests, docs;
- `query`: revision-backed `.tracy-stream` sessions and
  `SegmentTraceSource`.

No changes are pushed by this work.

## Godot

```text
path:   C:\CodeProjects\GodotProjects\godot
branch: feature-tracy
state:  clean tracked worktree
```

## TPS demo

```text
path:   C:\CodeProjects\GodotProjects\tps-demo
branch: feature-tracy-demo
state:  clean tracked worktree
```

Godot and TPS retain their prior MCP/Tracy instrumentation. They were executed
for the real D3D12 acceptance matrix but were not modified.
