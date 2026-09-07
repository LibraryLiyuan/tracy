# Candidate 调查与停止合同

## 本轮新增：帧、窗口和实际 MCP 回执

采用 candidate-policy-v3；候选的 frame_scope、thread_or_queue、metric、manifestation 一并进入调查绑定。frame_cost 与 stable_slow/sustained_pressure 是不同表现，不能因名字相同而只查一次。可复用已核验的源码和调用链，但必须分别查询这些表现的时间范围。

`observation_unit=l0_segment` 的 GPU 候选使用 `representative_intervals` 的时间范围、Queue 和真实 L0 引用；片段序号不是 Player.Frame 帧号。渲染和 UI 两段 L0 均保留，不推断整帧 GPU 总耗时。

每项 finding 增加 `investigation_receipts`：evidence_id、purpose、manifestation、entity_json_pointers；缺少完整 path 时提供 parent_chain_json_pointers。Evidence 保存原始 JSON-RPC tools/call 请求和响应的相对路径及 SHA-256，可在 `mcp_receipt` 中与展示用规范化数据分开保存。

CPU/GPU 查询须绑定实际线程/Queue、完整父路径或稳定签名和所需代表帧/时间区间。只记录 frame.get、摘要、源码说明或 Sampling 而不展开候选事件树，不能关闭 CPU/GPU 候选。时间范围查询若需声称属于某 Player 帧，必须引用实际 frame.get 回执作为 frame_anchor_evidence_id，并用 frame_anchor_json_pointer 指向返回的 Frame 元数据。

报告构建器检查请求/返回 ID、Trace ID、文件 hash、查询成功和分页状态、查询对象及代表范围。错误帧、其他线程、同名不同父路径、缺失请求、截断结果以及仅有结论文字，均不能视为完成。该校验是可审计工作流约束，不宣称能抵御人为伪造全部文件。

父路径不能用同名但无关节点凑齐：链中每个 child.parent_ref 必须等于上级 ref。失败查询不能写成 Supported/Confirmed。对实际 capability/domain unavailable，可保存三次不同 MCP 请求 ID、同一明确 signature/Frame/线程范围的失败回执，设置 `outcome=unavailable`，结论只能为 `Unresolved`；参数错误、超预算和随便一种 error 都不能替代能力缺失证明。

阶段报告显式设置 `report_status=in_progress`，仍校验已写 finding 的数值、回执和范围，完整列出未调查候选。生成器会显示未完成警告；不能把阶段报告改名后称为完整报告。默认 `complete` 对任何待查 selected 均拒绝。

完成一批调查后运行 `manage-analysis-state.py record-investigation-audit --state <state> --analysis <analysis-result> --candidates <candidate-manifest> --evidence <evidence-manifest>`，绑定三份文件路径与 SHA-256。随后重新 ingest 原候选页刷新队列。恢复和终态校验会重新检查实际回执；只填写 completed ID 不予放行。部分批次的 analysis-result 采用 in_progress，完整批次采用 complete。

质量仍为独立附录。尚未调查或有未解决的回执校验错误时，保存阶段记录；不能宣布完整报告通过。

## 1. 候选不是结论

Candidate Manifest 是 Query 根据全量确定性统计和 Profile 生成的调查入口。候选触发只说明“值得调查”，不等于根因成立。

AI 不得修改以下 Query-owned 事实：

- priority、trigger 和 trigger evidence；
- observed/threshold value、unit、scope 和 authority；
- representative frame 与选择理由；
- inclusive/exclusive/wait-critical 排名；
- frequency、percentile、Debt、峰值、容量和质量计数。

需要新的统计量时，向 Query 请求；不得由 AI、Python 或 PowerShell重算。

## 2. 队列和覆盖

强制调查：

- Query `selected=true` 的全部候选，无论优先级；
- priority=P1；
- triggers 包含 `user_focus`，无论其 selected 状态。

优先级只决定调查顺序，不决定是否可以跳过。未入选、非 P1/UserFocus 候选保留为可选 Backlog。

所有未完成 Candidate 同时保留在 Investigation Backlog。报告必须披露候选总数、强制总数、完成数、未调查数，以及未调查项的 priority/domain/trigger。恢复时以 candidate_id 去重，已完成项不再次调查。

执行顺序：P1 → UserFocus → 其他 selected；同级按 Candidate Manifest 稳定顺序。

P0 仅为兼容保留，当前 candidate-policy-v3 不生成 P0；不得将原 P1/P2 自动升级。Capture 质量项不属于 Candidate，不消耗队列/Backlog/覆盖计数，也不要求 L6。把 `capture_quality` 原样保存到报告附录，简述受影响域、原始原因和分析限制。只有某个性能候选的调查确实需要该缺失数据时，才在该候选下登记关联的 EvidenceGap 并进行必要补查。扫描器 checksum/identity/audit 错误仍然阻断，不作为普通质量附注放行。

## 3. 每个候选的固定分析路径

### 3.1 发现

保存 `candidate_get` 完整响应，说明由哪些 budget/top/anomaly/capacity/user_focus 路径触发。任何数值直接引用 trigger evidence。遇到旧 `quality` 候选必须重新生成策略结果，不执行其深度调查。

### 3.2 代表实例

调用 `representative_frames`。Query 的理由可能包括 worst、typical_bad、first、last、burst_start、burst_peak、burst_end、structure_change。不存在“固定12帧”；只调查 Query 真正返回的实例，证据冲突时再请求明确反例或相邻范围。

### 3.3 Limiter-first 查询

从完整 Frame 边界区分 Active Work、Wait 和 Intentional Pacing，再沿真实关系找到控制帧完成的线程、Job、Queue、GPU Pass、Allocation 或 I/O 请求。Top/最长实体只是假设入口。

### 3.4 假设账本

每项假设保存：

```text
hypothesis
required_evidence
mcp_queries
supporting_evidence
contradicting_evidence
source_evidence
status = Supported / Rejected / Unresolved
```

至少主动寻找一个反证。缺 typed relation、调用栈、Source、硬件计数器或完整生命周期时，记录最小 EvidenceGap。

### 3.5 源码核查

只有 Trace build revision 与源码 revision 匹配时，源码可支持 Confirmed。记录文件、行、符号、内容 hash、callstack/source provenance 和 revision match。错位源码只能解释机制候选。

### 3.6 结论

每个候选结果必须包含：

- Confirmed / Supported / Hypothesis / Unresolved / Rejected；
- Primary Limiter；
- 完整可审计 Analysis Trail；
- 支持证据和反证；
- 影响范围、频率和 Query 数值；
- 优化方向、功能风险和理论收益边界；
- 可执行复测指标；
- EvidenceGap（若未闭环）。

## 4. 深度 L0–L6

```text
L0 现象、触发、频率和预算/Top/异常事实
L1 Primary Limiter
L2 具体线程、Job、Pass、资源或请求
L3 跨线程、Queue或生命周期因果链
L4 调用栈、Source、View/Range/Subresource
L5 机制和随规模增长的变量
L6 优化方向、风险、理论上限和复测指标
```

强制候选应达到 L6；受证据限制时可停在最深可信层，但必须给出 blocked_by 和 minimum_additional_evidence，不能把假设升级为根因。

## 5. 关键归因规则

- Parent inclusive 和 Child exclusive 分开；禁止父子重复加总。
- CPU wall time、on-CPU time、Wait time 分开。
- 不同线程和不同 GPU Queue 不直接相加。
- GPU Pass 使用关系不自动证明 shader/bandwidth/cache/occupancy/overdraw 机制。
- referenced working set 不等于 owned/physical/resident bytes。
- 短期增长不自动等于泄漏。
- Catalog invalid 时资源级来源为 unavailable；DXGI/physical 可独立分析。
- GPU L0 缺 OriginFrameId 时不得伪称拥有完整单帧 GPU 边界；必须披露 frame attribution 限制。
- 单 Trace 固定 `TotalCaptureOverhead = NotMeasuredSingleTrace`。

## 6. 停止条件

满足以下条件才可结束正文调查：

1. 全部 selected/P1/UserFocus 已完成实际调查；质量附注不属于强制调查。
2. 证据受限者有实际 MCP 查询/失败回执、最深可信结论及最小 EvidenceGap；单独一条 Backlog 原因不算调查。
3. 所有数值能回到 Candidate/Evidence。
4. 所有 Confirmed 有精确关系、匹配源码或其他独立硬证据。
5. 所有 MCP 与源码查询进入 Ledger。
6. 未调查候选和未覆盖贡献已披露。

“时间不够”“已有若干结论”或“只看了12帧”不是有效停止条件。

遇到时间/资源限制可暂停：保存剩余队列和已核查证据，生成明确未完成的阶段结果。不得将此标成完整报告或 completed_with_degradation；后者表示完成调查但源数据受限，不表示调查尚未执行。
