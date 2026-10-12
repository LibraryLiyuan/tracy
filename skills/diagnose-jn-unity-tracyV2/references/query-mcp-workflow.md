# Tracy Query MCP 工作流

## 1. 单一常驻会话

用 `local-profile.paths.toolchain_root` 中的 `tracy-query.exe --mcp` 启动一个常驻进程。保存 executable SHA-256、PID/start identity、MCP initialize 结果和 capability 列表。一个分析任务不混用其他 Query build，不使用一次性 CLI 旁路。

顺序：

```text
initialize
→ tools/list
→ trace.open
→ trace.status 直到 ready/failed
→ system.capabilities
→ trace identity/context/coverage/quality
→ 分块查询
→ trace.close
```

Trace 状态未 ready 前不做性能查询。工具进程崩溃时保存 stdout/stderr、Windows crash evidence、最后请求和索引状态，再按重试规则重启同一 build。

## 2. Query Ledger

每次请求记录：

```text
sequence
method
canonical params
analysis window / cursor
attempt
started/ended/elapsed
response path + SHA-256
partial/truncated/next_cursor
query budget consumed/exhausted_by
trace id/revision/watermark
quality/warnings
```

MCP 原始响应不得直接倾倒进模型上下文。先落盘，再提取与当前 Signature 假设相关的最小证据。

## 3. 三次尝试

每个逻辑请求最多尝试 `project-profile.analysis.query_limits.max_attempts` 次：

1. 原始范围与合理预算。
2. 缩小时间范围、使用索引或分页。
3. 拆分实体/帧/关系查询，仍保持同一证据目标。

第三次仍失败：

```text
EvidenceGap.reason = QueryUnavailable
BlockedBy = method + failure
MinimumAdditionalEvidence = 可完成该假设的最小查询/索引/重录要求
```

禁止静默跳过或把失败解释为空数据。

## 4. 长 Trace 分页

不设置总帧上限。使用 Query Index 和时间窗口：

```text
窗口 A 分页 → 增量统计
窗口 B 分页 → 增量统计
…
→ 跨窗口 Signature 合并
→ Signature 级实例深查
```

- `deep_query_batch_size` 只限制单次实例请求。
- P50/P95/P99、频率和 Debt/minute 基于所有有效帧。
- 连续问题跨窗口时按稳定键和时间邻接合并。
- 保存每页 cursor、count、next_cursor 和完整性。
- 响应超过配置大小时分页或缩小字段，不截断后继续下结论。
- 并发遵守配置，默认避免多个大查询争用内存。

## 5. 查询路由

全局轻扫：

- trace identity/context/coverage/quality。
- FrameSet 和全量 Frame 分页统计。
- thread/queue、CPU/GPU zone 统计。
- Job、CPU memory、GPU memory、I/O、Sampling/Context Switch、Tracy Runtime 与 Tracy Analysis domain status。
- GPU Catalog status/validation。

只对达到显著性门禁的 Signature 使用相关 playbook 深查。无异常域只保存健康统计到附录，不强制生成正文卡片。

## 6. Evidence Manifest

每个被结论引用的响应都登记 Evidence ID：

- Trace ID/SHA-256 必须和报告一致。
- method/params、request/response path/SHA-256、attempt 必须完整。
- frame/time/thread/queue/entity refs 必须来自响应。
- `partial/truncated/next_cursor` 必须原样保存。
- Evidence data 使用相对路径，不能逃逸报告目录。

Renderer 只信 Evidence Manifest 和经过校验的数据文件，不信 AI 提交的布局坐标或父子关系。
