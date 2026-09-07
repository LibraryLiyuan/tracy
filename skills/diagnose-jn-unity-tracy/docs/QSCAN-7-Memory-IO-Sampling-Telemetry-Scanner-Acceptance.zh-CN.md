# QSCAN-7 Memory / I/O / Sampling / Telemetry 扫描器验收记录

## 1. 结论

状态：**Passed（阶段轻量门禁）**。

QSCAN-7 已把 CPU allocation、GPU 显存口径、I/O 生命周期、Sampling、Context Switch 和 Tracy 自身 telemetry 计数转换为后续精确统计可消费的结构化事实。实现明确隔离了两类容易误导 AI 的口径：GPU working set 不会被当成 physical/owned bytes；单次 Trace 中的 producer 自报成本不会被扩张成“已测得 Tracy 总开销”。

## 2. CPU Memory

- CPU Pool 与 `gpuD3D12` Pool 分开扫描。
- 输出 allocation/free 数、累计申请/释放、峰值 live bytes、capture 结束 live bytes。
- 地址复用要求前一生命周期已经释放；重叠生命周期标记 `AccountingGap`。
- 同时间 free/create 使用半开区间语义，先 free 后 alloc，不制造虚假峰值。
- 分类包括：`Capacity`、`Growth`、`Churn`、`LeakCandidate`、`Transient`、`AccountingGap`。
- capture 起点已有的 allocation 记作 open boundary，不自动判为泄漏。
- 只有在 capture 内观察到 create、达到寿命门槛、且地址生命周期无歧义时，才成为 LeakCandidate。
- 先完成整池地址生命周期核验，再计算 LeakCandidate，结果不依赖事件输入顺序。

## 3. GPU Memory

- Catalog 状态区分 `Complete`、`Invalid`、`Disabled`、`Absent`。
- physical allocation 按 AllocationId 去重；多个 Resource 共享同一 Allocation 时物理字节只计一次。
- 分开输出：
  - `physicalBytes`
  - `residentBytes`
  - `ownedPhysicalBytes`
  - `logicalCapacityBytes`
  - `rangeEvidenceBytes`
  - `maximumDirectWorkingSetBytes`
  - `maximumInclusiveWorkingSetBytes`
  - DXGI sampled usage
  - `untracked=max(DXGI-physical,0)`
- Catalog Invalid 时停止声明 Resource/Pass/Range 来源事实，但保留可独立证明的 physical allocation 总量。
- alias/shared allocation 只作为关系与质量事实，不重复累加 physical bytes。

## 4. I/O

- Request 状态分为 `Complete`、`Cancelled`、`Error`、`Orphan`、`Incomplete`。
- 保存 queue latency、execution time、total time、requested/transferred bytes 和 stage count。
- 多 terminal、truncated 或时间逆序会降低 exactness，不用空结果隐藏。

## 5. Sampling 与 Context Switch

- Sampling 按 thread role、sample kind、callstack 和叶函数聚合。
- callstack 以有界批次解析；无法解析的样本保留为 `<unresolved>` 并单独计数。
- Context Switch 区间按 Tracy 实际语义计为线程运行时间。
- wakeup→start 单独计为 ready wait；waiting 与 preemption 由持久化 reason/state 分类。
- Sampling 或 Context Switch 权限不可用时，结果为 `available=false + unavailableReason`，不能解释成“没有热点/调度问题”。

## 6. Tracy Telemetry Cost

- 解析 `JNQ1` producer counter 首末快照，验证计数单调性。
- 输出 producer observed/emitted/event bytes/cpu time/drop/overflow/mismatch/unresolved/degrade。
- 证据等级分为：
  - `MeasuredSelfCost`：producer 自报 CPU time。
  - `ObservedSystemPressure`：drop、overflow 或 degrade 等实际压力。
  - `EstimatedAttribution`：事件数、字节数和 FrameImage 数量形成的归因线索。
  - `RequiresABValidation`：整体捕获开销必须用 A/B 证明。
- 固定输出：

```text
TotalCaptureOverhead = NotMeasuredSingleTrace
```

- Tracy worker CPU、FrameImage CPU/GPU 成本与 capture 写盘成本如果没有独立持久化计数，分别返回明确 unavailable reason。
- producer 出现 drop、overflow、mismatch、unresolved、degrade 或 tail truncation 时，质量状态不完整。

## 7. Synthetic 门禁

| 检查项 | 结果 |
|---|---|
| CPU 累计 alloc/free | 780 / 360 bytes，Passed |
| CPU peak/end live | 630 / 420 bytes，Passed |
| 地址正常复用 | 1，Passed |
| 地址重叠 AccountingGap | 1，Passed |
| open boundary | 1，未误判为 LeakCandidate |
| 干净长寿命 allocation | LeakCandidate，Passed |
| shared GPU Allocation | physical 只计 300 bytes，未重复为 400 |
| GPU logical capacity | 220 bytes，保持独立 |
| direct/inclusive working set | 100 / 300 bytes，保持独立 |
| DXGI / untracked | 350 / 50 bytes，Passed |
| Catalog Invalid | physical=300，Resource facts unavailable |
| Catalog Disabled/Absent | 状态准确 |
| I/O Complete/Cancel/Error/Orphan | 各 1，Passed |
| Sampling resolved/unresolved | 3 / 1，Passed |
| Context Switch running/ready-wait | 30 / 8 ns，Passed |
| 权限不可用 | 显式 reason，Passed |
| producer CPU/event/bytes | 1000 ns / 8 / 256 bytes |
| producer drop/overflow | 1 / 1，质量降级 |
| 单 Trace 总开销 | `NotMeasuredSingleTrace` |
| Worker/Session 两类 source | 核心结果一致 |
| 最大扫描批次 | ≤2 |
| QSCAN-1～7 轻量回归 | 8/8 Passed |

## 8. 测试记录

红灯阶段：测试目标按预期因 `TracyMemoryIoSamplingTelemetryScanner.hpp` 不存在而编译失败。

实现后执行：

```powershell
cmake --build build-query-scan --config Release `
  --target tracy-memory-io-sampling-telemetry-scanner-tests -- /m:1

ctest --test-dir build-query-scan -C Release `
  -R '^tracy-(query-contract|query-deterministic-scan-contract|analysis-profile|neutral-aggregate-store|bounded-scan-cursor|frame-cpu-scanner|gpu-job-managed-scanner|memory-io-sampling-telemetry-scanner)$' `
  --output-on-failure
```

结果：

```text
8/8 Passed
Total Test time: 0.49 sec
```

受控 QSCAN-7 测试二进制 SHA-256：

```text
3A24FED07255FB795E797B8BA64E50C53AC601D91B201A2BB17A1C1B1EBB349D
```

## 9. 已知边界

- 本阶段只生成中性事实，不使用预算或 Top 规则判断性能问题；精确分位数与异常分类在 QSCAN-8。
- `ownedPhysicalBytes` 是唯一有效 physical allocation 的所有权口径；面向具体 Resource/Pass 的 owner rollup 仍由 Catalog relation 和后续候选查询完成。
- DXGI usage 是采样值，不能与事件级 physical peak 当作同一时间精度。
- working set 是 Pass 内去重引用集合；不同 Pass 或兄弟节点之间不能直接相加。
- 单次 Trace 能证明 producer 自报 CPU time、事件量、丢失和系统压力，不能证明“如果不录 Tracy 会快多少”。
- 真实 Worker `.tracy`、N30 Session 与 Profiler 的集中差分留在 QSCAN-13。

## 10. 阶段身份

基线：

```text
9c845fef QSCAN-6 Add GPU Job managed and relation scanners
```

目标提交：

```text
QSCAN-7 Add memory IO sampling and telemetry scanners
```
