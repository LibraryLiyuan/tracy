# Query 1.35 原生扫描与恢复工作流

> 2.2 适用范围：先执行 [优先级与测试人员决策](priority-and-tester-review.md)。本文的原 P 级为 Query 旧编号；所有“源码必读/深查完成”要求仅适用于新 P0、原有低级别必查项及测试人员明确批准的新 P1。新 P1 入队项先完成 Query 证据核查和候选报告；未选择不算源码受阻。分流排除项保留精确 Query 事实及理由。旧流程章节仅用于其适用范围内的证据和源码合同，不能覆盖新分流规则。


## 1. 目标和权威边界

Query 原生扫描负责遍历整个 Trace、构建中立聚合、计算精确统计并按 Profile 生成 Candidate Manifest。Skill 不重复这些计算。

```text
Trace（本发行版仅支持普通 `.tracy`）
→ Neutral Aggregate（与预算无关）
→ Policy Evaluation（由 normalized Profile 决定）
→ Candidate Manifest
→ AI directed investigation
```

中立聚合由 Trace 强身份、Query EXE SHA-256、Query 1.35、扫描算法和 Aggregate schema 标识。只改变 Profile 时必须复用聚合，只重算 Policy。

## 2. 唯一常驻 MCP

一个分析任务只连接一个 `tracy-query.exe --mcp` 进程。启动时必须提供已授权的 `--analysis-root` 和位于其中的 `--analysis-cache-root`。记录：

- executable 绝对路径和 SHA-256；
- PID、创建时间和 stdout/stderr；
- MCP initialize 结果；
- tools/list；
- `tracy_describe` 返回的 Query schema 和 scan capability。

Query schema 必须精确等于 `1.35.0`。不允许用 `tracy-query.exe --request` 或其他一次性 CLI 作为正式查询旁路。

退出条件：MCP 可响应、schema 匹配、`tracy_scan` 具有 profile_validate/start/status/cancel/resume/summary/signatures/candidates/candidate_get/representative_frames/quality/close。

失败：保存进程和 MCP 错误，状态记为 `mcp_unavailable`，停止分析。

## 3. Profile 与 Trace

1. 用安全 YAML loader读取 `config/JNTracy.AnalysisProfile.yaml`。
2. 调用 `tracy_scan(operation="profile_validate", profile=<parsed mapping>)`。
3. 保存 normalized Profile、Profile identity 和请求/响应 SHA-256。
4. 用 `tracy_trace_open` 打开一个已完成转换的 `.tracy`。
5. 轮询 `tracy_trace_status`，直到 ready 或 failed。
6. 读取 identity、context、coverage、quality 和 capabilities。

退出条件：Trace ready、强身份稳定、Profile valid、输入域状态已记录。

失败：invalid Profile、identity mismatch、输入不受支持、Trace failed 或核心校验失败均停止。不存在的域为 `present=false`；失效域保留 invalid reason。

## 4. Scan 生命周期

调用：

```text
tracy_scan(operation="start", trace_id, profile)
→ scan_id
→ tracy_scan(operation="status", scan_id)
```

状态处理：

| 状态 | 动作 |
|---|---|
| Queued/Validating/Scanning/Aggregating/EvaluatingPolicy/Auditing | 持久化进度，稍后继续 status |
| Complete | 读取 summary/quality/candidates |
| CancelledResumable | 使用同一 scan_id 调用 resume |
| Failed | 保存错误并停止，不读取候选 |

不要按固定时长判断完成。用户要求暂停或任务必须让出资源时，调用 `cancel`，等待 `CancelledResumable` 后结束当前运行。

Query 进程异常退出后：重启相同 EXE build，以原 scan_id 调用 status。若返回 CancelledResumable，则 resume；若 Complete，直接复用；身份或 checksum 不匹配则停止。同一源码重新编译也可能改变 EXE SHA；新 EXE 拒绝旧 scan identity 属于正常隔离。继续既有调查时保留并使用生成缓存的原始已验证 EXE，不手改 identity、不以合并提交相同代替二进制身份。

退出条件：`completed=true` 且 state=Complete。

## 5. Aggregate、Candidate 和分页

Complete 后依次调用：

1. `summary`：保存 Aggregate/Candidate content SHA、backlog 计数以及独立的 `capture_quality`；确认 `policy_algorithm=candidate-policy-v3`。当前缓存使用 `native-scan-v3`，保留完整逐帧磁盘序列与独立帧/窗口/全局入口；不复用旧版代表帧上下文，也不把有效 GPU 局部观测混入完整 L0 的分母。
2. `quality`：保存总扫描和各域完整性。
3. `candidates`：limit 不超过1000，首页省略 `cursor` 或使用字符串 `"0"`，不能显式传空字符串。之后原样使用非空 `next_cursor`，直到 `done=true` 且 `next_cursor=null`；验证 ordinal 与累计返回条数连续。
4. 必要时 `signatures`：只取调查需要的字段/签名，不把全量结果送入模型。

每页原始响应先原子落盘，再把路径、SHA、cursor、count、partial/truncated 和 scan identity 写入 MCP Ledger。总数、重复 candidate ID 或 cursor 不连续时，分页不完整，不能进入调查。

`scripts/manage-analysis-state.py ingest-candidates` 逐页读取已落盘候选、只保留精简调查元数据，不读取 Trace、不做统计。完整 trigger/代表区间留在原始 MCP 页，以 candidate ID 点查，不在导入器中物化全表。所有未选候选仍保留 Backlog，selected/原P1/UserFocus 的分流登记不减项，源码队列按新级别和测试人员决定单独管理；分页缺口、重复 ID、partial 或提前 done 不能发布新状态。精简队列和 ID 集合仍随候选数增长，不宣称整个 Skill 已实现常量内存或完整报告渲染已分块。

`capture_quality` 不属于候选分页，必须从 summary/quality 原样带入 Analysis Result 和报告附录；质量项不进入 P0/P1/P2、调查配额或性能 Backlog。旧策略候选缓存不能沿用：新版 Scan identity 包含 policy algorithm，旧 Scan 不能恢复；按当前 EXE/策略重新开始（可验证身份的中立聚合才可复用）。不要改写旧 Manifest、优先级或旧报告。Quality 的扫描器自身审计失败继续阻断；源数据失效只限制相关结论。具体性能调查依赖缺失域时才做对应质量深查。

退出条件：Candidate Manifest 分页完整，content identity 与 summary 一致，全部 Candidate 可按 ID 查询。

## 6. 三次查询策略

每个定向证据目标最多三次：

1. 原始代表范围和必要字段。
2. 缩小时间/Frame 范围、分页或使用索引。
3. 拆分实体、关系或数据域，但保持同一问题。

实际能力缺失是例外：按 candidate-investigation 的 unavailable 合同，对同一已定位目标保存三个不同请求 ID、相同 method/params 的失败回执，以成功实体锚点绑定录制身份。不能在这个分支改变参数后仍宣称三次同目标尝试。

三次均失败时登记：

```text
EvidenceGap.reason = QueryUnavailable
blocked_by = method + last_error
minimum_additional_evidence = 最小可恢复查询/索引/重录要求
```

失败、partial、truncated、invalid、分页未完成不能转为空集合或健康结论。

## 7. 响应落盘和上下文控制

每个请求记录：

```text
id, sequence, tool, operation/method, canonical params,
attempt, started_at, ended_at, elapsed,
request_path/sha256, response_path/sha256,
trace_id, scan_id, aggregate/candidate identity,
cursor, next_cursor, partial, truncated, quality, warnings
```

模型上下文只读取当前 Candidate 所需的小型响应片段。完整响应保留在 Evidence 目录。单响应超过限制时分页或缩字段，不截断后继续下结论。

## 8. Close

报告校验成功后：

1. `tracy_scan(operation="close", scan_id)` 释放 Query 持有的 TraceSource pin。
2. `tracy_trace_close`。
3. 保存最终 state、输出 manifest 和工具身份。

若报告失败，保留 scan 和 Trace 状态以供恢复，不先 close 后丢失调查上下文。
