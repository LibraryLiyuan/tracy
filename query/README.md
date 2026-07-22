# Tracy Query v1

`tracy-query` is a read-only, headless query and MCP process for saved `.tracy` captures. It does not use ImGui, open network connections, create a database, or call an LLM.

The protocol contract is frozen in `schema/tracy-query-v1.schema.json`. `schema/coverage-v1.json` maps every persisted Tracy 0.13.1 data domain to its v1 query surface and test status.

Current executable surface:

```text
tracy-query --version
tracy-query --schema
tracy-query --doctor [--trace file.tracy]
tracy-query --trace file.tracy --request request.json|-
tracy-query --trace file.tracy --batch requests.ndjson|-
tracy-query --mcp
```

Trace paths are denied unless their canonical final path is below an `--allow-root`. With no explicit root, only the current working directory is allowed. Requests are limited to 1 MiB, ordinary responses to 8 MiB, and list methods use stable trace/revision/filter-bound cursors.

`--request` and `--batch` are implemented. `--mcp` is wired in the next milestone. Until every coverage entry is marked `complete`, the v1 implementation must not be described as complete.
