# Tracy Query v1

`tracy-query` is a read-only, headless query and MCP process for saved `.tracy` captures. It does not use ImGui, open network connections, create a database, or call an LLM.

The protocol contract is frozen in `schema/tracy-query-v1.schema.json`. `schema/coverage-v1.json` maps every persisted Tracy 0.13.1 data domain to its v1 query surface and test status.

The executable and end-user documentation are added in subsequent implementation milestones. Until every coverage entry is marked `complete`, the v1 implementation must not be described as complete.
