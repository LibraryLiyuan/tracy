# QSCAN-13 Query 原生扫描真实 Trace 阶段验收

日期：2026-09-07  
状态：PassedWithSourceQualityDegradation（短 Trace / Legacy Worker 验收通过；Session 验收由用户明确延期）  
Tracy 分支：`feature/JNTracy-AI-QueryScan`  
阶段开始 HEAD：`2ea94e86620f67d767cc55da7b3e66123ed068b4`  
Query 契约：`1.35.0`  
Skill 草案：`diagnose-jn-unity-tracy` 2.0.0

## 0. 最终验收边界（2026-09-07）

用户明确要求先使用短 Trace 验收 AI 性能分析流程，不再把耗时很长的 N30 Session 扫描作为本阶段门禁。本节及第 4.5、6、7 节的最终状态覆盖本文早期保留的 30 分钟 Session 执行记录。

本次最终输入：

```text
C:\Users\Admin\Documents\JN-Unity-T3\QSCAN13-Short-Skill-Acceptance-20260907\
oracle-existing\GPUCatalogOff-HighEvidence-20260903-202313.tracy
```

输入 SHA-256：

```text
DA5495E1BC5D85962BB0F9BDB2CABDC41E35C51EA0F1576A9EAB702F36C15211
```

最终结论：

- Query-native 扫描、Candidate Policy、必要候选调查、报告生成、确定性重建和报告契约均通过；
- 15 个必须调查项已完成 15 个，余下 1,356 个候选完整保存在 backlog，没有被静默丢弃；
- Query 对所有已消费域保存 input/consumed count、checksum 和质量状态，`unreported_gap_count=0`；
- 源 Trace 自身的 CPU、GPU、GPU Catalog、Managed、Relation 和 Telemetry 质量缺陷被忠实保留，因此状态为 `PassedWithSourceQualityDegradation`，不是无条件 `Passed`；
- N30 Session、matched A/B 采集开销测量和 QSCAN-14 安装发布均不属于本次完成门禁。

## 1. 本阶段目标

QSCAN-13 不再只验证 synthetic fixture，而是使用真实 JN Unity HighEvidence 数据证明以下合同：

- Query 常驻 MCP 可以完成一次确定性全域扫描；
- `.tracy` 和 N30 Session 都能使用同一套扫描接口；
- 大输入使用有界分页和流式聚合，内存不随高频事件数线性膨胀；
- 中断后可以复用相同 Scan ID 和已经提交的 Aggregate；
- Query 产生中性 Aggregate，配置化 Candidate Policy 在其后独立执行；
- 扫描器自身不得把源 Trace 已声明的无效域伪装为完整；
- MCP 响应、内存和磁盘临时产物满足门禁。

## 2. 实现期间发现并修复的规模问题

### 2.1 Session 分页退化为 O(N²)

症状：同一数据族的第 N 页读取仍从该族开头逐条跳过 `offset`，页数增长后单页耗时持续上升。30 分钟录制会把正常的线性扫描放大为近似二次复杂度。

根因：Session Reader 已经具备按 storage order 或稳定 ID 定位的索引，但全范围、无过滤的扫描路径没有使用这些索引。

修复：

- CPU Zone、GPU Zone 使用稳定 ID 直接寻址；
- Memory、Sampling、Scheduling、Message、Plot、Lock 使用 storage order 直接寻址；
- 带线程、时间、Context、Pool 等过滤条件的路径继续保留原始精确语义；
- 为全部数据族增加“完整读取结果 == 多页读取拼接结果”测试。

### 2.2 Sampling/Context Switch 全量 DTO 常驻

症状：G05 Session 含 `23,701,345` 条 Sample。旧实现把全部 `SampleDto` 读入 vector 后再聚合，Query 峰值私有内存达到约 `8.95 GiB`。

根因：扫描器使用 `ReadAll<SampleDto>` 和 `ReadAll<ContextSwitchDto>`，内存与事件总数线性增长；而最终结果只需要按线程角色、Sample Kind、Callstack 聚合。

修复：

- 增加分页 visitor；
- Sample 每页读取后立即聚合，只保留唯一 Callstack ID 和聚合键；
- Context Switch 每页读取后立即按线程角色聚合；
- 扫描结束后只解析唯一 Callstack；
- unresolved 计数从聚合 count 计算，保持原有输出语义。

结果：G05 峰值私有内存从 `9,610,928,128` 降至 `2,690,170,880` 字节，下降约 `72.0%`；峰值 Working Set 从 `9,124,896,768` 降至 `2,364,616,704` 字节，下降约 `74.1%`。

### 2.3 统计与签名重复工作

真实 Trace 同时暴露出稳定 SourceLocation、分类 token 和相邻同值样本在热路径中被反复字符串化的问题。本阶段进一步完成：

- 数值 registry/token 路径，避免热循环反复格式化稳定字符串；
- CPU 稳定 SourceLocation signature cache；
- CPU/GPU/Job/Managed 扫描视图，避免 Manager 复制完整事件结果；
- 相邻同值统计样本在窗口内合并；
- lexical 排序恢复，确保优化前后确定性输出不变。

### 2.4 Session serial clock 在部分数据域失步

症状：同一份真实 Trace 通过 Legacy Worker 和 N30 Session 查询 CPU Memory 时，公开事件数与语义存在差异。

根因：`JnGpuZoneBeginCallsite` 属于 serial-clock 编码事件，Session Memory Reader 没有在读取该事件时前进 serial clock；Lock Reader 的同类路径也没有保持全局时钟消费规则。后续事件因此使用了错位的时间基准，被错误地纳入或排除。

修复：

- Memory 和 Lock Session Reader 对所有 serial-clock 事件统一执行时钟前进；
- 添加混合 GPU Zone / Memory / Lock 事件回归 fixture；
- 对真实 `Admin-Preflight` 执行 Worker 与重建 Session 的 Memory MCP 语义对比。

结果：两种介质均返回 `21,072` 条 Memory 事件，规范化多重集 SHA-256 完全相同：

```text
111f2f9e1b54ba11bf94b2a80e42dad6469619f065ad728ea8167cecf35953a0
```

证据：

```text
C:\Users\Admin\Documents\JN-Unity-T3\QSCAN13-Acceptance-20260906\
oracles\fixed-memory\Admin-Preflight.Memory.semantic-diff.json
```

### 2.5 Continuous FrameSet 末尾帧被当作完整帧

症状：Continuous FrameSet 的最后一个 FrameMark 没有下一个 FrameMark 提供真实结束边界，但旧扫描器仍可能将 Reader 合成的尾部时间当作完整帧。

修复：CPU Frame 扫描器对 Continuous FrameSet 只接受已被下一 FrameMark 证明结束的帧；末尾帧保留为边界状态，不进入 complete-frame 统计。已添加专用回归测试。

### 2.6 Domain Audit 误把聚合输出数当作源输入数

症状：`scan-quality.json` 的 Memory `input_count` 显示 `20,315`，与公开 Memory 事件数 `21,072` 不一致。

根因：Manager 将“CPU memory pool 摘要数 + GPU allocation 摘要数”当作 Memory 源事件数。这是审计元数据错误，不是扫描丢数据，但会误导后续 AI 判断。

修复：每个 Scanner 在真实 `ReadAll`/`VisitAll` 输入处记录源事件数，并将审计域拆分为 `memory`、`gpu_memory`、`gpu_catalog`、`io`、`sampling`、`scheduling` 和 `telemetry`。

真实 Preflight Session 复测结果：

| Domain | input_count | consumed_count | 状态 |
|---|---:|---:|---|
| memory | 21,072 | 21,072 | complete |
| gpu_memory | 20,315 | 20,315 | complete |
| gpu_catalog | 635,815 | 635,815 | complete |
| io | 50 | 50 | complete |
| sampling | 1,758,767 | 1,758,767 | complete |
| scheduling | 362,698 | 362,698 | complete |
| telemetry | 357 | 357 | complete |

该次扫描耗时 `73.967 s`，峰值私有内存 `393,895,936 bytes`，最大 MCP 响应 `6,830,930 bytes`，`unreported_gap_count=0`。

### 2.7 GPU/Job 每帧分母混入其他 FrameSet

症状：代码审查发现 GPU 与 Job 的中性统计共同使用 `cpu.completeFrameCount`。该字段是 `Player.Frame`、`Render.Frame`、`Editor.Frame` 等所有 FrameSet 的完整帧数之和，会系统性压低 GPU/Job 的每完整帧均值、P95 贡献率和候选排序。

根因：CPU 扫描器只向 Manager 暴露全局完整帧总数及“已有 CPU signature”的分母，没有暴露每个 FrameSet 自身的精确完整帧数。Manager 随后又用 `max(total, 1)` 掩盖分母不可用状态。

修复：

- CPU 扫描结果新增逐 FrameSet 的名称、引用、continuous 属性和 exact complete count；
- GPU `GPU.Frame` 与 Job `Player.Frame` 只使用名称严格等于 `Player.Frame` 的完整帧数；
- 多个同名 `Player.Frame` FrameSet 明确求和，其他 FrameSet 不参与；
- 存在 GPU/Job 输出但没有可证明的 `Player.Frame` 时返回 `player_frame_denominator_unavailable`，禁止退化为 `1`；
- 新增一个包含 1 个有效 Player 帧和 1 个 Render 帧的回归测试，修复前 Job 分母为 2，修复后必须为 1。

定向 Release 测试结果：

```text
tracy-frame-cpu-scanner-tests.exe      exit 0
tracy-analysis-scan-manager-tests.exe  exit 0
```

发现缺陷时正在运行的 `session-30m-final-v1` 使用旧 Query，已按精确 PID 终止并完整保留中间现场；它不进入本次短 Trace 验收结论。

### 2.8 Job originFrameId 跨 FrameSet 污染

症状：修复每 FrameSet 分母后，真实短 Trace 仍出现 Job 代表帧和统计分母不一致。Job 事件携带的 `originFrameId` 可能来自 Present/Render 等非 Player FrameSet；仅判断 ID 非零会把这些 Job 纳入 Player 帧统计。

修复：

- 先由 `Player.Frame` 的 Begin/End 构造已证明完整的 Player Frame ID 集合；
- Job 只有在 `originFrameId` 属于该集合时才参与 Player per-frame 统计；
- 回归 fixture 同时包含 Player Frame Job 和 Present Frame Job，明确断言后者不进入 Player 统计；
- 修复前测试失败、修复后通过，随后才执行最终真实短 Trace 扫描。

该修复使最终 Candidate 数从旧结果 1,370 更新为 1,371；这是语义修正后的新确定性结果，不是扫描不稳定。

## 3. 全量自动测试

2026-09-07 使用 Release 配置、单 MSBuild worker 完整重编译后执行 CTest：

```text
18/18 Passed
Total Test Time: 45.07 sec
```

覆盖：Query 合同、确定性扫描合同、Profile、Aggregate Store、Bounded Cursor、CPU/GPU/Job/Managed/Memory/IO/Sampling/Telemetry 扫描器、Exact Statistics、Candidate Policy、Persistent Scan Manager、GPU Analysis、Session Store/Inventory 和 MCP Transcript。

Skill 草案同步执行：

```text
Python unittest:       35/35 Passed
PowerShell scripts:    Passed
Distribution contract: Passed
Skill linter:          0 error, 3 warning, 5 info
```

当前 Query EXE SHA-256：

```text
E9BB2FB126E087E3D958F3F08F2CBC1400BAA89BAF010BA6984CC8FB54B308E0
```

## 4. 真实 Trace 结果

### 4.1 小型真实 Trace

输入：`G-GpuCatalogOff-HighEvidence-30s.tracy`

| 指标 | 结果 |
|---|---:|
| 扫描时间 | 22.31 s |
| Bottleneck Signature | 2,244 |
| Candidate | 774 |
| MCP 最大单响应 | 6,847,794 bytes |
| 峰值私有内存 | 462,209,024 bytes |
| 峰值 Working Set | 445,788,160 bytes |

中断/恢复复测：

- 主动 Cancel 后恢复成功；
- 恢复后复用相同确定性 Scan ID；
- Signature 与 Candidate 数量完全一致；
- `.work` 临时目录无遗留。

### 4.2 GPU Catalog On HighEvidence `.tracy`

输入：`D-GpuCatalogOn-HighEvidence-10s-Direct.tracy`

| 指标 | 结果 |
|---|---:|
| 扫描时间 | 19.82 s |
| Bottleneck Signature | 2,287 |
| Candidate | 788 |
| MCP 最大单响应 | 6,848,140 bytes |
| 峰值私有内存 | 390,266,880 bytes |
| 峰值 Working Set | 357,195,776 bytes |

结论：GPU Catalog 开启的真实 HighEvidence Legacy Worker 路径能够完成全域扫描；所有输入 count 与 consumed count/checksum 一致，`unreported_gap_count=0`。该短 Trace 的 Sampling/Context Switch 不在源文件中，按 `absent` 返回；CPU、GPU、Managed、Relation 和 Telemetry 的源数据缺陷按具体 reason 标记为 invalid，没有伪装为 complete。

### 4.3 GPU Catalog Off 大型 `.tracy`

输入：`GPUCatalogOff-HighEvidence-20260903-202313.tracy`

| 指标 | 结果 |
|---|---:|
| 最终 Scan ID | `scan-0c75572ab42a92690c3b199946162fb3` |
| 扫描时间 | 1,144.29 s（19.07 min） |
| Bottleneck Signature | 6,621 |
| Candidate | 1,371 |
| 必须调查 / 已完成 | 15 / 15 |
| Backlog | 1,356 |
| MCP 最大单响应 | 6,827,162 bytes |
| 峰值私有内存 | 7,981,821,952 bytes |
| 峰值 Working Set | 7,907,430,400 bytes |
| Aggregate SHA-256 | `315d26a152fa41f5e5a4c0877b5f5b65ef229702e0de88410cc27ed7f349273e` |
| Candidate SHA-256 | `1d0a6fac54838a7f908102eea0da4c21499cabb06b52dd13d7b48d667d059260` |

结论：低于 `16 GiB` 硬门禁，也低于 `8 GiB` Working Set 目标，但仍接近目标上限。该输入走 Legacy Full Worker，打开阶段本身需要大内存；这不阻塞短 Trace Skill 验收，也不构成 N30 Session 已通过的证据。

同一 analysis cache 的有界 MCP 复查能够立即复用同一个已关闭 Scan ID，没有重新扫描。`memory.gpu.summary` 在限定窗口内未返回，驱动按精确 PID 终止 Query；该项被记为 Query 延迟/可取消性后续问题，不以空结果伪装成功。

### 4.4 G05 N30 Session

输入：`G05-Player-HighEvidence-20260831-173806.jn-trace-session`

| 指标 | 优化前分页版本 | 流式 Sample/CS 版本 |
|---|---:|---:|
| 扫描时间 | 1,225.69 s | 1,188.59 s |
| Bottleneck Signature | 6,646 | 6,646 |
| Candidate | 1,379 | 1,379 |
| 峰值私有内存 | 9,610,928,128 | 2,690,170,880 |
| 峰值 Working Set | 9,124,896,768 | 2,364,616,704 |
| MCP 最大单响应 | 6,827,404 | 6,827,404 |

正确性对比：

- Aggregate 内容 SHA-256 完全一致：`8106e152552318d1e6a1c95baa28b066ceade8bdc1967efaa071c9ede15da365`；
- 1,379 个 Candidate 排除由工具身份派生的 `candidate_id` 后，语义差异为 0；
- Candidate manifest 的 content identity 因 Query EXE SHA-256 属于算法身份而变化，这是设计行为，不是数据差异；
- 扫描完成时 `unreported_gap_count=0`，没有遗留 `.work`。

输入事件数：

| 数据域 | 数量 |
|---|---:|
| CPU Zone | 28,007,491 |
| GPU Zone | 3,674,791 |
| Job | 2,583,833 |
| Relation | 12,692,368 |
| Sampling | 23,701,345 |
| Memory | 348,170 |
| I/O | 29,068 |
| Managed | 25,520 |

源数据显式质量状态：

- CPU：`invalid_zone_timing=863`、`child_outside_parent=3041`、`missing_parent_zone=2231`、`missing_zone_end=6`、`zone_unassigned_to_complete_frame=9`；
- GPU：`gpu_temporal_frame_ambiguous=2682964`、`gpu_zone_unassigned=991827`、`gpu_child_outside_parent=12821`、`missing_gpu_zone_end=632`；
- Managed：`script_stack_incomplete=2`；
- Relation：`relation_single_value_ambiguous=25520`；
- I/O、Job、Memory、Sampling、Scheduling、Telemetry 为 complete。

以上是源 Session 已明确报告的数据质量，不是 Query 扫描器丢失。扫描整体 `complete=true` 且 `unreported_gap_count=0`；报告与 AI 必须保留这些 invalid 状态，不得把相关域升级为 Exact。

### 4.5 30 分钟 N30 Session（延期，不是本次门禁）

此前启动的 30 分钟 Session 扫描属于旧验收边界。用户已明确决定在另一个对话中单独验收 N30 Session，原因是该路径耗时很长，并且尚有独立的 Session 功能验收工作。

本次不使用 30 分钟 Session 的运行结果为短 Trace Skill 2.0 背书，也不把其未完成状态算作失败。保留的旧现场只用于后续 N30 对话续查。

## 5. 已确认的介质边界

本节记录开发过程中已经得到的介质事实，但 N30 Session 的完整功能、性能和跨介质验收已从本次短 Trace 门禁中移除。

- G05 的 Legacy `.tracy` 在 Full 和 Indexed Compact 打开路径均超过 `16 GiB`；驱动按硬门禁主动终止，没有继续冒险消耗系统内存。
- 同一数据的 N30 Session 能完成扫描；流式聚合后峰值私有内存约 `2.51 GiB`。
- 因此，现阶段 Legacy `.tracy` 可以验收短 Trace AI 分析流程；大型/长时间录制是否正式切换为 Session，等待独立的 N30 验收结论。
- 这不是丢弃数据或降低分析范围，而是把 TraceSource 从全量 Worker 切换为有界磁盘 Session Reader。

### 5.1 严格跨介质 Oracle 的当前边界

跨介质最强 Oracle 是“同一 N30 Session 导出局部 `.tracy`，再对 Session 与导出 Trace 执行同一扫描”。该项属于后续 N30 验收；QSCAN-13 不伪造结论。

已完成的跨介质证据是：

- 短 Trace 的 Legacy Worker 与 Session Reader 回归 fixture 全域差分；
- 真实 `Admin-Preflight` 的 Worker/Session CPU Memory 域语义完全一致；
- G05 扫描优化前后 Aggregate SHA-256 和 Candidate 语义完全一致；
- 每个域保存 input/consumed count、checksum 和 quality，任何未报告差异使扫描失败。

这些证据可以验证当前 Query-native 扫描的正确性，但不替代尚未可执行的 Session-export 强 Oracle。

### 5.2 Cancel/Resume 的精确边界

本阶段已经验证：Cancel 状态响应、`CancelledResumable` 持久化、同一 Scan ID Resume，以及最终 Aggregate/Candidate 的确定性结果。

当前实现的恢复单位是“已原子发布的 Neutral Aggregate”，不是 CPU/GPU 扫描器内部的某个分页位置：

- Aggregate 已完整发布：Resume 或不同 Policy 可直接复用；
- Aggregate 尚未发布：Resume 以同一 Scan ID 从安全起点重新扫描；
- 未提交 `.work` 不属于权威结果，不得被当成已恢复进度。

因此，QSCAN-13 的 Cancel/Resume 结论是正确性与状态机通过，不代表长扫描中段能够节省已消耗的扫描时间。若以后需要分页级续跑，应独立增加 Scanner cursor、run manifest 与跨域 checkpoint，不能从当前阶段级 checkpoint 推断已经具备该能力。

## 6. 当前门禁结论

| 门禁 | 状态 |
|---|---|
| Release 完整构建 | Passed |
| 18 项 CTest | Passed |
| Skill Python/PowerShell/Distribution | Passed |
| 小型真实 Trace | Passed |
| Cancel/Resume | Passed |
| GPU Catalog On HighEvidence `.tracy` | Passed |
| GPU Catalog Off 大型 `.tracy` | PassedWithTargetWarning（硬门禁通过） |
| G05 Session 完整性 | Passed |
| G05 优化前后语义一致性 | Passed |
| G05 Session 内存目标 | Passed |
| 最终短 Trace Query-native 扫描 | Passed |
| 必须候选调查 15/15 | Passed |
| 报告数值/代表帧/证据契约 | Passed |
| 报告确定性重建（10 个 Markdown） | Passed |
| Skill linter | PassedWithWarnings（0 error） |
| 30 分钟 Session | DeferredByUser_SeparateConversation |
| QSCAN-13 总状态 | PassedWithSourceQualityDegradation |

## 7. 发布边界

- 当前改动在短 Trace 门禁通过并完成最终 diff 审核后提交；不再等待 30 分钟 Session。
- 已安装 Skill 未修改。
- 本次只对草案运行规范 linter：0 error、3 warning、5 info；没有自动添加 LICENSE、README 或 CI。
- QSCAN-14 的 evaluator、草案到安装目录发布，以及安装后真实报告演练没有获得授权，未执行。

## 8. 最终报告与已知限制

最终主报告：

```text
C:\Users\Admin\Documents\JN-Unity-T3\QSCAN13-Short-Skill-Acceptance-20260907\
report\Performance-Analysis-Report.md
```

验收状态机：

```text
report\short-skill-acceptance.json
report\engineering-acceptance.json
report\skill-lint.json
```

报告中保留以下限制：

- `Main.PlayerLoop` P95 为 `35.96184585 ms`，但 exclusive P95 仅 `0.1389227 ms`；由于 CPU/GPU/Relation 源质量失效，不能把 wrapper 直接判为根因；
- GPU Catalog 的运行时配置实际为 enabled/degraded，不能依据文件名中的 `GPUCatalogOff` 推断其关闭；
- 捕获结束时四个 GPU physical pool 合计 `5,953,748,992 bytes`（`5.954 GB`，`5.545 GiB`），这只是结束状态，不是峰值；
- Catalog Core invalid 且 `memory.gpu.summary` 有界查询未返回，因此不提供资源级显存和峰值的权威结论；
- Candidate 代表帧当前未保留 CPU FrameSet 身份，已在报告中作为人工导航缺口披露；
- 单次 Trace 不能测量 Tracy 总体采集开销，必须另做 matched A/B。
