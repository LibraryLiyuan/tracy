# Repository and worktree state

Baseline observed on 2026-07-23 before implementation. The completed
compatibility-streaming milestone is delivered as a local-only commit on
`feature-stream`; it is not pushed or merged.

## Tracy

Common repository:

```text
C:\CodeProjects\GodotProjects\tracy-0.13.1
```

Worktrees:

| Worktree | Branch | HEAD | State |
| --- | --- | --- | --- |
| `C:\CodeProjects\GodotProjects\tracy-0.13.1` | `feature-StructuredData` | `75cce700` | index reports one modified Markdown file; content diff is empty |
| `C:\Users\Admin\Documents\JN-Unity-T3\tracy-stream-worktree` | `feature-stream` | based on `75cce700`; local delivery commit | isolated streaming work |
| `C:\Users\Admin\Documents\JN-Unity-T3\tracy-structured-worktree` | `feature-StructuredData-implementation` | `5659ee62` | separate existing worktree |

The main Tracy worktree reports:

```text
M query/docs/acceptance-results.md
```

`git diff --exit-code -- query/docs/acceptance-results.md` returns `0`; Git also
warns that LF would become CRLF. This is most likely an index/stat or line-ending
observation. It was deliberately not refreshed, normalized, reset, or otherwise
touched because it belongs to the pre-existing worktree.

The streaming changes exist only in `feature-stream`. The delivery commit is
local; nothing was pushed, merged, or applied to either other Tracy worktree.

## Godot

```text
path:   C:\CodeProjects\GodotProjects\godot
branch: feature-tracy
HEAD:   2d3e26b53196a505ea066c14d5e98134bcf586f3
state:  clean tracked worktree
```

## TPS demo

```text
path:   C:\CodeProjects\GodotProjects\tps-demo
branch: feature-tracy-demo
HEAD:   cddb00ba98c95bf15dea277066a617bdca7b7506
state:  clean tracked worktree
```

The existing MCP instrumentation in Godot/TPS was used through its built
executables and saved traces. Neither repository was modified.
