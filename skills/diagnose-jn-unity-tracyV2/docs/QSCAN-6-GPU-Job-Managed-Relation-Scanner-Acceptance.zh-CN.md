# QSCAN-6 GPU / Job / Managed / Relation 扫描器验收记录

## 1. 结论

状态：**Passed（阶段轻量门禁）**。

QSCAN-6 已把 GPU Queue 树、Job 生命周期、C#/Lua Source Stack 与 typed relation 变成可供后续精确统计和候选策略使用的结构化事实。阶段实现遵循“证据等级不能升级”的原则：GPU 时间重叠不会被写成确定性 Frame 归属；无 waiter 的 Job 不会生成虚假 continuation；缺失或不完整的 Script Stack 仍保留 Zone 时间，但明确返回 unavailable reason。

## 2. GPU 事实

- Direct、Compute、Copy 按真实 `GpuContext` 分开维护 Zone stack。
- Queue 内按全深度计算 inclusive、direct-child interval union 和 exclusive。
- Track A 保存真实物理 Queue 事实。
- Track B 保存逻辑 Frame 归属及证据等级。
- Frame 归属严格分为：
  - `exact_frame_relation`
  - `inferred_from_child_passes`
  - `temporal_candidate`
  - `unassigned`
- Temporal candidate 只在 GPU Context 已校准时产生。
- 两个 L0 Segment 即使属于同一个 Frame，仍是两个物理 Segment，不伪装为一个跨 CommandList Zone。
- 子 Pass 具有一致精确 FrameId 时，父节点可标记为 `inferred_from_child_passes`；该状态不会伪装成 direct exact relation。

## 3. Job 与 Managed 事实

- Job 输出 Schedule、Ready、Queue、FirstRun、Complete、Execution 与 Wait 分解。
- 保留 dependency JobId、origin FrameId、Schedule callsite/callstack 与 provenance。
- packed handle 拆分为 slot/generation；受控 fixture 验证同 slot 跨 generation 复用不会串代。
- waiter 和 continuation 独立判断；没有 waiter 的异步 Job 不生成返回线事实。
- Script Stack 要求存在 Header、声明深度匹配且全部 FrameDefinition 可解析。
- C# 与 Lua direct Source Stack 保留函数、文件和行号。
- 栈不完整返回 `script_stack_incomplete`；引用不存在的栈返回 `script_stack_missing`。
- ZoneEnd 缺失不丢掉 ZoneBegin 时间事实，而是进入质量项。
- `ManagedProducerKind` 明确区分 direct Source Stack 与 Unity ProfilerMarker；本阶段 binary Script Zone 属于 direct 路径，ProfilerMarker 时间线由 CPU Scanner 保留。

## 4. Typed Relation

- 只消费实际持久化的 relation，不由时间共现生成 exact relation。
- Source/Target kind 为 Unknown 时降低 exactness。
- `LogicalParent`、`OwnedBy`、`ContinuesAs` 按单值关系验证。
- 同一 source 的单值关系出现多个 target 时，所有冲突边标为 ambiguous/non-exact，并保存代表 relation refs。

## 5. Synthetic 门禁

| 检查项 | 结果 |
|---|---|
| Direct/Compute/Copy Context 分轨 | Passed |
| Direct 父 inclusive/child-union/exclusive | 30 / 20 / 10，Passed |
| 多个 L0 对同一 Frame | 保持独立物理片段 |
| Compute 父由唯一子 Pass 推断 | `inferred_from_child_passes` |
| Copy 仅时间相交 | `temporal_candidate`，未升级为 exact |
| 范围外 GPU Zone | `unassigned` |
| Job Schedule→Complete→Wait | Passed |
| 无 waiter Job | 无虚假 continuation |
| packed handle generation reuse | 不串代 |
| C# Source Stack | Complete |
| Lua 声明2帧但只有1帧 | `script_stack_incomplete` |
| Zone 引用未知 Stack | `script_stack_missing` |
| Typed OwnedBy 双目标 | 两条均 ambiguous/non-exact |
| 最大扫描批次 | ≤2 |
| 生产 Query 链接 | Passed |
| 当前完整 CTest | 14/14 Passed |

## 6. 测试记录

红灯阶段：测试目标按预期因 `TracyGpuJobManagedScanner.hpp` 不存在而失败。

实现后执行：

```powershell
cmake --build build-query-scan --config Release \
  --target tracy-gpu-job-managed-scanner-tests tracy-query --parallel 1

ctest --test-dir build-query-scan -C Release --output-on-failure -j 1
```

结果：

```text
14/14 Passed
Total Test time: 6.08 sec
```

最终受控测试二进制 SHA-256：

```text
93A4FB441D45364BB9EB8DDABBDE44BB743F1B58D72A246D9D5449527DDD3435
```

## 7. 已知边界

- 本阶段形成事实与证据等级，不按预算或 Top 规则判定性能问题；Policy 在 QSCAN-9 执行。
- Temporal GPU Candidate 不能用于确定性的整帧 GPU 总耗时，只能进入调查候选。
- Track A/B 当前以结构化事实 DTO 表达；QSCAN-8 执行外排聚合和 Neutral Audit。
- UnityMarker callback 的时间事实来自 QSCAN-5 CPU Zone；direct Script Source Stack 来自本阶段，两者在后续 Aggregate 中按 provenance 合并，不能互相冒充。
- 真实 Queue/Pass/Frame Relation 与 Profiler 的人工差分在 QSCAN-13 集中验收。

## 8. 阶段身份

基线：

```text
019ede94 QSCAN-5 Add exact frame and CPU zone tree scanner
```

目标提交：

```text
QSCAN-6 Add GPU Job managed and relation scanners
```
