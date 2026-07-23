# ChatGPT desktop and Codex MCP setup

`tracy-query --mcp` is a local STDIO MCP server. ChatGPT desktop or Codex starts the executable, writes JSON-RPC requests to stdin, and reads structured results from stdout. ChatGPT web cannot directly launch a local executable; remote HTTP MCP is outside v1.

Example trusted-project or user configuration:

```toml
[mcp_servers.tracy]
command = "C:\\CodeProjects\\GodotProjects\\tracy-query-build\\Release\\tracy-query.exe"
args = [
  "--mcp",
  "--allow-root", "C:\\CodeProjects\\GodotProjects",
  "--allow-source-root", "C:\\CodeProjects\\GodotProjects"
]
startup_timeout_sec = 10
tool_timeout_sec = 300
enabled = true
default_tools_approval_mode = "auto"
```

Place it in `~/.codex/config.toml` or a trusted project's `.codex/config.toml`. The build never edits that configuration automatically.

The MCP server exposes 12 workflow tools rather than every internal method:

1. `tracy_trace_open`
2. `tracy_trace_status`
3. `tracy_trace_close`
4. `tracy_describe`
5. `tracy_overview`
6. `tracy_search`
7. `tracy_inspect`
8. `tracy_timeline`
9. `tracy_analyze`
10. `tracy_compare`
11. `tracy_validate`
12. `tracy_job`

`tracy_inspect` has two compatible forms. The concise `domain`/`operation`
form remains the preferred drill-down interface. For complete coverage, it also
accepts an exact public Query `method` plus a nested `params` object:

```json
{
  "method": "hardware_sample.events",
  "trace_id": "trace-1",
  "params": {
    "address": "0x7ff600001000",
    "kind": "cache_misses",
    "limit": 100
  }
}
```

Call `tracy_describe` with `operation` set to the exact method first when the
required parameters are unknown. `system.describe` and MCP routing share
`QueryMethodRegistry`; transcript tests exhaust the registry so a newly added
public Query method cannot silently remain unreachable from MCP. The
`system.schema` method returns the envelope schema plus embedded domain, field,
and MCP coverage manifests for machine-readable self-audit.

All tools are declared read-only, non-destructive, and closed-world. They return both `structuredContent` and compact JSON text. Tool execution errors use `isError=true`, allowing the model to correct arguments and retry.

MCP protocol versions `2025-06-18` and `2025-11-25` are supported over one-line UTF-8 STDIO framing. The server implements initialize, initialized notification, ping, tools list/call, resource list/templates/read, logging level, progress, and cancellation. Long analyses run as cooperative jobs; call `tracy_job status`, then `tracy_job result`, or request cancellation.

Resources are fetched only when needed:

```text
tracy://trace/{trace_id}/source/{source_id}
tracy://trace/{trace_id}/symbol-code/{symbol_id}
tracy://trace/{trace_id}/frame-image/{image_id}
```

Embedded source is preferred. External source is disabled unless its canonical path is inside an `--allow-source-root`. Frame images are decoded from Tracy BC1 data and encoded as PNG on demand.

The recommended model sequence is: open → poll ready → overview → validation → outliers/hotspots/memory/locks/scheduling → bounded drill-down → callstack/symbol/source → cross-check → cite fingerprint and refs. `tracy-query` supplies evidence; the model writes the natural-language diagnosis.
