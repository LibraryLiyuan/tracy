# G01 单 Trace 深度诊断回归

本报告由固定生成器从已验证的 Analysis Result 与 MCP Evidence 生成。Markdown 是权威结论；SVG/PNG 是静态证据附件。

## 1. Trace 身份与证据质量

- Trace ID：`trace-1`
- Trace SHA-256：`0083175da9ff46f8f92417f6e7fdcc2cf2cf1a36cd28f4eb35b62a74d5085e57`
- 分析范围：`user_selected`，`133448161153`–`133850025884` ns
- 有效帧：1；排除帧：0
- Trace 质量：`completed_with_degradation`
- 配置身份：
  - `local_profile`：`config/local-profile.json` / `8b798bc684fa0b8413c593a70a6e7ce37ff9e4c7f1304b653f08f5fcaf0a04aa`
  - `marker_attribution`：`config/marker-attribution.json` / `f47ebee0d73f945469c0e40c07e9453266f75a18b1ab29ead433d30b9c1bbbed`
  - `performance_budgets`：`config/performance-budgets.json` / `98a97952fa346c39efc269f55ae8ab61c592f15942f90fab6b2f79ffaccf6023`
  - `project_profile`：`config/project-profile.json` / `d4b1fa80124b7ef41191f07993a40e9aa8aaa7bd62bc6b573bbd00ff7569dd9a`
- 质量警告：frame.explain exhausted max_edges and max_nodes on attempt 3

## 2. 性能结论总览

- **Frame.Unknown/Unknown/Unresolved.MainPlayerLoopWallClock** — Unknown；频次 1；累计 Budget Debt 385.198 ms；结论 `Unresolved`。

## 3. Frame Class 分布

| Frame Class | 帧数 | 比例 | Flags |
|---|---:|---:|---|
| IsolatedSpike | 1 | 100.000% | BudgetMiss, DataQualityDegraded, UserSelected |

## 4. Bottleneck Signature 地图

| Signature | Primary Limiter | 严重性 | Debt/min | 深度 | 置信度 |
|---|---|---|---:|---|---|
| [Frame.Unknown/Unknown/Unresolved.MainPlayerLoopWallClock](#card-sig-g01-unresolved-long-frame) | Unknown | critical | Unavailable | L1 | high_for_symptom_low_for_cause |

## 5. 关键路径与等待传播

- 没有已证明的跨线程或跨 Queue 因果链；相关结论应见 Evidence Gaps。

## 6. 独立 Evidence Cards

<a id="card-sig-g01-unresolved-long-frame"></a>
### Frame.Unknown/Unknown/Unresolved.MainPlayerLoopWallClock

- 影响：严重性 `critical`；频次 1；累计 Debt 385.198 ms。
- Primary Limiter：`Unknown`；Secondary：无。
- 深度：`L1`；置信度：`high_for_symptom_low_for_cause`；状态：`Unresolved`。
- 静态 Timeline：未生成（没有包含可验证 Zone 坐标的 Timeline Evidence）。
- 因果链图：未生成（没有已证明的 typed relation）。

代表实例：

- `worst`：Frame 281474976711184；Evidence EVD-G01-FRAME-527-PARTIAL

支持证据：

- 该完整帧墙钟为 401.864731 ms
- Main.PlayerLoop 覆盖该帧绝大部分墙钟

反证与未支持结论：

- 不能由截断的 frame.explain 确认 Primary Limiter
- 不能把 Main.PlayerLoop 的 inclusive wall time 当作 Main CPU active time

假设账本：

- `Unresolved` Main.PlayerLoop 的长墙钟由 Render/GPU 下游等待传播控制；支持：EVD-G01-FRAME-527-PARTIAL；反证：无。

调用栈 / Source provenance：

- SiteReusedNative

优化方向：

- 先取得未截断的相关时间线与 typed wait relation，再按实际 limiter 选择优化点

功能风险：

- 在 limiter 未解析前优化最长 Marker 可能优化错误子系统

- 理论收益上限：Unavailable；依据 `unresolved_critical_path`。
- 复测指标：完整 relation 覆盖率；Primary Limiter 可解析率；Player.Frame P95
- EvidenceGap：`UnresolvedWaitChain` / `QueryTruncated:max_edges,max_nodes`；最小补充证据：分页或缩小窗口后的完整 correlated timeline, Main→Render→GPU typed relation。

## 7. CPU 内存与 GPU 显存专项

### 7.1 CPU 内存

- 未形成达到显著性门禁的 CPU 内存结论。

### 7.2 GPU 显存

- 未形成达到显著性门禁的 GPU 显存结论。

## 8. Tracy 运行时采集与离线分析压力

- `TotalCaptureOverhead = NotMeasuredSingleTrace`

### 8.1 Tracy Runtime

- 本 Fixture 不能独立测量总体录制开销

### 8.2 Tracy Analysis

- 未发现 Query、转换或 Profiler 的独立离线分析压力。

## 9. Evidence Gaps

- `QueryTruncated`：完成 frame relation 分页或缩小查询范围

## 10. 优化优先级和复测方案

1. `Frame.Unknown/Unknown/Unresolved.MainPlayerLoopWallClock`：先取得未截断的相关时间线与 typed wait relation，再按实际 limiter 选择优化点。复测：完整 relation 覆盖率；Primary Limiter 可解析率；Player.Frame P95。

## 11. 全域健康统计附录

| 域 | 状态 | Evidence |
|---|---|---|
| CPU | partial | EVD-G01-FRAME-527-PARTIAL |
| GPU | present_not_deeply_resolved | EVD-G01-FRAME-527-PARTIAL |
| GPU Memory | resource_nodes_present_not_validated_by_this_fixture | EVD-G01-FRAME-527-PARTIAL |
| Job | present_not_deeply_resolved | EVD-G01-FRAME-527-PARTIAL |

## 12. 查询审计索引

详见 [Analysis-Process-and-Query-Audit.md](Analysis-Process-and-Query-Audit.md) 与 [Evidence-Index.md](Evidence-Index.md)。
