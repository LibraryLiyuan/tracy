# Security and privacy boundary

## Filesystem boundary

- `--allow-root` controls which `.tracy` files may be opened. Without it, only the current working directory is allowed.
- Paths are canonicalized before comparison, and only regular files with a `.tracy` extension are accepted. This blocks `..`, symlink, and Windows junction escapes.
- Files are opened read-only. No recorder, database, sidecar, browser, shell, GUI, or network client is started.
- External source has a separate `--allow-source-root`; it is disabled by default and revalidated when read.

## Untrusted trace data

Zone/message names, app info, file paths, source, and machine-code-derived text are capture data—not server instructions. Tool results identify these fields as untrusted. Invalid UTF-8 is replaced with U+FFFD during protocol serialization. Input depth, request size, response size, page size, callstack depth, source bytes, and image dimensions all have hard limits.

## What remains local and what does not

The `.tracy` file is not uploaded wholesale by this process. It remains on the machine and is read by the local Worker. However, after the model calls a tool, that bounded JSON result—and any requested source snippet or frame image—enters the ChatGPT/Codex context and may be processed by the configured service. “Local MCP” therefore does not mean that every returned analysis value is guaranteed never to leave the machine.

Use the narrowest practical roots, avoid enabling external source for unrelated trees, close sessions after analysis, and review organization data policies before exposing captures containing secrets or user data.

## Resource and concurrency limits

At most two trace sessions are active, loads are serialized, query dispatch is serialized, and derived analysis caches default to 512 MiB with LRU eviction. Worker memory is separate. Up to four analysis jobs can be queued/running; cancellation is cooperative at bounded scan checkpoints. An allocation failure or response-budget violation becomes `RESOURCE_LIMIT`, not a process crash.
