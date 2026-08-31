# 单 Trace limiter-first 分析工作流

## 1. 身份与质量先于结论

开始分析前保存：

- Trace ID、fingerprint、SHA-256、外层格式、协议、JN section 和 Query schema。
- capture profile、连接/断连、SessionEnd、stream/trace 完整性和右删失状态。
- process、Unity、engine、package、Tracy、DLL/PDB、INI/config hash。
- 各 domain 的 `present/status/reason`、dropped、truncated、overflow、degrade 和 quality。
- 四份配置路径与 SHA-256。

capability 缺失不是空结果。`partial`、`truncated`、分页未完成或第三次查询失败必须进入 EvidenceGap。

## 2. AnalysisWindow 与全量扫描

默认窗口是整个完整 Capture。只有用户给出帧号或时间范围时创建局部 `AnalysisWindow`。不要根据 Marker 名称猜测登录、战斗或加载等语义窗口。

使用 Query Index 按时间窗口分页：

```text
完整 Frame 列表
→ 分块统计
→ 累计 P50/P95/P99、Budget Debt、Flags 和质量
→ 合并跨窗口连续事实
```

保存每个窗口的时间边界、游标、返回数量、完整性、重试次数和下一游标。统计必须基于全部有效帧；代表实例只用于深查。

`project-profile.analysis.deep_query_batch_size` 只限制一次 MCP 深查请求携带的实例数，不限制 Signature 数、总帧数或报告问题数。

## 3. Frame Class

Primary Class：

```text
StableWithinBudget
StablePressure
RecurrentSpike
IsolatedSpike
Transition
Unclassified
```

正交 Flags：

```text
BudgetMiss
IntentionalPacing
LoadingActivity
CapturePerturbed
FrameImageCheckpoint
GpuEvidenceCheckpoint
IncompleteBoundary
DataQualityDegraded
UserSelected
```

规则：

- `StablePressure` 表示持续接近或越过配置预算的稳定压力，不等于单个离群峰值。
- `RecurrentSpike` 的最小重复次数读取 `spike_detection.recurrent_min_count`。
- `Transition` 只由明确状态证据或用户窗口确认，不凭帧时形状命名。
- `CapturePerturbed` 保留在完整分布，并从稳定业务分布单独隔离。
- 不完整边界和质量降级帧不能支持否定性结论。

## 4. Bottleneck Signature

稳定键：

```text
Domain / Limiter / Mechanism.StableEntity
```

键中不得出现帧号、时间戳、pointer、一次性 Job/CommandList/Submission ID、动态对象名、易变参数或开发版本号。实例 ID 只进入 Evidence。

任一条件命中即成为独立 Signature 和 Evidence Card：

1. 稳定预算失败。
2. 位于帧完成关键路径。
3. 重复次数达到配置门槛。
4. 每分钟累计 Budget Debt 达到 `significance.debt_per_minute_frame_budget_multiplier × frame.hard_budget_ms`。
5. 孤立实例达到 `significance.isolated_spike_frame_budget_multiplier × frame.hard_budget_ms`。
6. 内存/显存容量、增长、churn、碎片或核算异常。
7. Tracy Runtime 插桩/采集、Sampling、写盘或 Tracy Analysis Query 成为明显热点。
8. 用户明确指定。

不按 Top N 裁剪。

## 5. 代表实例

每个 Signature 独立选择：

- `typical`：最接近该簇中位状态。
- `worst`：Budget Debt 最大。
- `repeated`：来自另一时间区段。
- 状态型问题追加 `first` 和 `recovery`。
- 存在相似 Marker 但没有造成问题时追加 `counterexample`。

默认先深查前三类。证据矛盾时增加实例，不因已有其他 Signature 的代表帧而跳过。孤立峰值只查询真实存在的实例。

## 6. Primary Limiter

对每个慢帧簇执行：

```text
Player.Frame 边界
→ Active Work / Wait / Intentional Pacing
→ 控制帧完成的线程或 Queue
→ Schedule / Dependency / Wait / Fence / Submission / GPU Relation
→ Primary Limiter
```

枚举：

```text
IntentionalPacing
MainThreadCpuWork
MainWaitsJob
MainWaitsRender
RenderThreadCpuWork
JobWorkerCpuWork
RenderWaitsGpuFencePresent
GpuCriticalPath
LoadingIo
CpuMemoryPressure
GpuMemoryPressure
OsSchedulingCpuContention
TracyRuntimeOverhead
TracyAnalysisPressure
Mixed
Unknown
```

归因规则：

- Main→Render→GPU 等待链归于实际控制帧完成的 GPU 工作。
- Main 等 Job 时区分 Job 长执行、Ready/Queue 延迟、Worker 饱和、负载不均和 OS 调度。
- VSync、帧率限制或显式 sleep 归为 IntentionalPacing。
- CPU 与 GPU 独立超预算且都影响帧完成时归为 Mixed。
- 缺少真实 Relation 返回 `Unknown` 并记录 `UnresolvedWaitChain`。
- 最长 Zone、Top Marker 和 Sampling 热点只建立候选假设。

## 7. 假设账本和 L0–L6

每个 Signature 保存：

```text
Hypothesis
RequiredEvidence
MCPQueries
SupportingEvidence
ContradictingEvidence
Status = Supported / Rejected / Unresolved
```

深度：

```text
L0 现象、频率、Budget Debt
L1 Primary Limiter
L2 具体线程、Job、Pass 或资源
L3 跨线程、Queue 或生命周期因果链
L4 调用栈、Source、View/Range/Subresource
L5 机制与增长变量
L6 优化方向、功能风险、理论收益上限、复测指标
```

每张卡达到 L6。缺证据时仍填写 L6 的可执行边界，但必须同时记录 `DeepestLevel`、`BlockedBy` 和 `MinimumAdditionalEvidence`，不能把假设写成根因。

## 8. CPU 内存与 GPU 显存结论

固定分类：

```text
CapacityPressure
GrowthTrend
Churn
LeakCandidate
TransientSpike
WorkingSetPressure
Fragmentation
AccountingGap
```

CPU 链：

```text
预算/趋势 → allocation 类型和大小 → 生命周期 → 创建调用栈 → Source
```

GPU 链：

```text
DXGI/Physical → Allocation/Heap → Resource 类型与名称
→ View/Range/Subresource → GPU Pass → Create Callsite
```

强制区分 `owned_bytes`、`physical_bytes`、`resident_bytes`、`referenced_working_set`、`direct_range_bytes`、`inclusive_range_bytes` 和 `untracked_bytes`。Catalog 无效时只允许 DXGI/physical 结论，并记录资源来源缺口。

## 9. 单 Trace Tracy Runtime / Analysis 边界

检查 producer CPU time、事件/字节、队列积压、Sampling、Context Switch、FrameImage、GPU Reference、Catalog、写盘吞吐、drop、overflow、degrade 和 Tracy 函数关键路径。

单 Trace 固定返回：

```text
TotalCaptureOverhead = NotMeasuredSingleTrace
```

该值表示没有未录制对照，不表示开销为零。
