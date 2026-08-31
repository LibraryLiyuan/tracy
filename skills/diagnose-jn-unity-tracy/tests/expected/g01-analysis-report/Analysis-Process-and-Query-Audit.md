# Analysis Process and Query Audit

| Evidence ID | MCP 方法 | Attempt | Quality | Partial | Truncated | Data |
|---|---|---:|---|---|---|---|
| `EVD-G01-FRAME-527-PARTIAL` | `frame.explain` | 3 | truncated | True | True | `evidence-data/frame-explain-527.json` |

每个 MCP 请求最多尝试 3 次；第三次失败必须以 QueryUnavailable EvidenceGap 出现在主报告。
