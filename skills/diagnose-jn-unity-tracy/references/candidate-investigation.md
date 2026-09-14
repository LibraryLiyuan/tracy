# Candidate 调查与停止合同

> 2.2 适用范围：先执行 [优先级与测试人员决策](priority-and-tester-review.md)。本文的原 P 级为 Query 旧编号；所有“源码必读/深查完成”要求仅适用于新 P0、原有低级别必查项及测试人员明确批准的新 P1。新 P1 入队项先完成 Query 证据核查和候选报告；未选择不算源码受阻。分流排除项保留精确 Query 事实及理由。旧流程章节仅用于其适用范围内的证据和源码合同，不能覆盖新分流规则。


> 2.1流程补充：必须同时执行 deep-analysis-and-report.md。下文保留的Candidate Finding与收据完成条件仅证明查询覆盖；所有深查完成条目都必须读实际源码。所有报告正文改用可读名称；本文出现的candidate_id、ref、SHA及Evidence字段名属于机器合同，不能直接作为报告展示内容。源码不可读时保留阶段结果，不以Unresolved自动完成整项深查。


## 本轮新增：帧、窗口和实际 MCP 回执

采用 candidate-policy-v3；候选的 frame_scope、thread_or_queue、metric、manifestation 一并进入调查绑定。frame_cost 与 stable_slow/sustained_pressure 是不同表现，不能因名字相同而只查一次。可复用已核验的源码和调用链，但必须分别查询这些表现的时间范围。

`observation_unit=l0_segment` 的 GPU 候选使用 `representative_intervals` 的时间范围、Queue 和真实 L0 引用；片段序号不是 Player.Frame 帧号。渲染和 UI 两段 L0 均保留，不推断整帧 GPU 总耗时。

`representative_frames` 对 GPU 可合法返回空数组，不能据此跳过调查；以 `candidate_get.representative_intervals` 为准。`l0_complete=false` 限制的是完整片段/整帧归因，不抹掉已明确有效的局部事件成本。`when_present` 与完整 L0 子集分布分别引用，完整子集中的零值不能用来声称整个捕获中该 Pass 无成本。

GPU 代表区间的 `event_refs` 可能仅指向 L0，而非最终候选 Pass。必须从该 L0 沿 `structural_signature` 的准确父子路径展开；获取根树不等于已调查叶节点。GPU 节点同时携带 CPU producer `thread_ref` 与 GPU `context_ref`，Queue 身份只使用后者（或 `gpu_context_ref`）。对 ref-based `zone.gpu.tree`，使用 `interval_anchor_evidence_id` 和 `interval_anchor_json_pointer` 绑定独立 `zone.gpu.get` L0 回执：ref 必须来自该代表区间，GPU 起止时间完全匹配，叶节点在该区间内且有已核验祖先链。不得向不支持的接口添加虚构时间参数、把 L0 ordinal 写成 Player.Frame，或把当前 GPU 片段锚点声明为完整 GPU 帧边界。

每项 finding 增加 `investigation_receipts`：evidence_id、purpose、manifestation、entity_json_pointers；缺少完整 path 时提供 parent_chain_json_pointers。Evidence 保存原始 JSON-RPC tools/call 请求和响应的相对路径及 SHA-256，可在 `mcp_receipt` 中与展示用规范化数据分开保存。

真实 `zone.cpu.tree` / `zone.gpu.tree` 可能只返回 root 和 children，不带祖先。此时逐级以 `zone.cpu.get` / `zone.gpu.get` 获取祖先，用 `parent_chain_evidence=[{evidence_id, entity_json_pointer}]` 按根到直接父节点顺序绑定独立回执。每条祖先请求 ref 必须等于返回 ref；连续 parent_ref、Trace、线程/Queue 和完整 path 一并核验。不能把补取的父节点手写进原 MCP 响应。FrameSet 根候选由严格的 `signature_id == "frame-wall:" + frame_scope` 身份识别，不能只凭名字识别。有 CPU Zone 的代表区间必须绑定真实完整根树与 frame.get；确实无记录的代表区间使用下述窄分支。

### FrameSet 区间已核验无 CPU Zone

仅限 CPU FrameSet duration 候选（`observation_unit=frame` 且上述 frame-wall 身份完全匹配）。每个缺失代表帧必须保存：

- 同 Trace 的真实 `frame.get`，请求与响应 FrameSet/index 相符。
- 准确 begin/end 范围的 `zone.cpu.search` 首页请求，无名称、线程、路径等过滤；不得只查最后一页。
- 成功响应 `data.zones=[]`，returned/omitted 均为 0，omitted 精确，next_cursor=null，partial/truncated=false、无预算耗尽。`tree` 的 children=[] 或 returned=0 不证明没有根 Zone。
- `investigation_receipts` 使用 `purpose=zone_tree,outcome=verified_absent`，绑定 `frame_anchor_evidence_id` 与 `/data`，不编造 entity ref。
- Finding 只能为 `Unresolved`；顶层 `evidence_gaps` 必须包含同 candidate_id、reason=`NoRecordedCpuZonesInRepresentativeFrame`、搜索与 Frame 锚点 evidence_refs、blocked_by 和 minimum_additional_evidence。

这只完成“该代表区间已实际查询但无法归因”的调查，不证明 CPU 成本为零或该段健康。该候选其他有记录的代表帧仍需正常展开树。具体 CPU/GPU Zone、错误查询、超预算、未完分页或人为过滤出的空集不能使用此分支；采集质量仍是附录，只有该候选依赖的缺口进入其调查。

### 具体 CPU 候选在代表帧未记录

使用 `purpose=zone_tree,outcome=verified_not_recorded`，结论仅为 `Unresolved`。它不同于整个 FrameSet 无 Zone：其他线程或其他路径仍可能有大量工作。

必须保存真实 `frame.get` 和完整 `zone.cpu.search`：搜索恰好覆盖该代表帧的 begin/end，候选 FrameSet/index 与请求、返回一致；搜索无过滤，或唯一名称过滤为 `{mode:exact,text:候选叶节点原名}`。禁止使用 Query 忽略的 `thread_ref`、自造 signature 过滤、从中间页开始或只交最后一页。分页使用已验证的完整 `page_evidence_ids` 链；所有返回节点均须纳入检查。

按真实结果处理：

| 返回情况 | 需要的证明 |
|---|---|
| 完整空集合 | returned/omitted=0、明确耗尽、无 partial/truncated/预算耗尽 |
| 同名只在其他线程 | 每个返回节点的原始 thread_ref 明确不同；不能把缺少线程字段当作不同 |
| 同线程同名、父路径不同 | 原始完整 path，或 `exclusion_evidence=[{ref,parent_chain_evidence:[...]}]` 逐个绑定独立 `zone.cpu.get`，核验名字、ref、线程、父链接直至 parent_ref=null |

根叶节点在原回执明确 `parent_ref=null` 时，空祖先链就是完整结构；缺少 parent_ref 字段不是根。开放祖先的 ref/name/parent_ref 可用于结构核对，原 complete=false/end=null 保留，不用于耗时或完成判断。任一返回节点与目标线程及完整路径一致，或仍有无法排除的节点，此分支失败。

收据绑定 `frame_anchor_evidence_id` 和 `/data`。历史 `frame.get` 使用名字时，要么另取已有 opaque 请求作锚点，要么用 `frame_set_evidence_id` / `frame_set_json_pointer` 指向同 Trace 真实 `frame.sets` 的 name/ref 对；不从 ref 后缀或调用者自填名字推导 FrameSet。

顶层 `evidence_gaps` 必须具有同 candidate_id、reason=`CandidateNotRecordedInRepresentativeFrame`、全部搜索页/Frame锚点/排除父链（及名字映射）的 evidence_refs，以及具体 blocked_by、minimum_additional_evidence。其他有记录的代表仍要展开真实树。无事件只是这一范围的记录事实，不说明该帧健康、CPU开销为零或整个捕获缺失。

### Native Wait 没有记录完成端关系

仅对 `domain=cpu,metric=wait` 使用 `purpose=wait_completion,outcome=completion_not_recorded`。结论只能为 `Unresolved`，实际等待树、帧/线程/路径、全部代表以及可用采样/栈仍须调查；这个收据不提供代表覆盖。

`relation_evidence_ids=[出向ID,入向ID]`，且 evidence_id 为出向ID。两份真实 `relation.search` 分别只用 `source_kind=cpu_zone` 和 `target_kind=cpu_zone` 加受支持的预算/limit参数；不可加 namespace、时间、线程或其他缩小条件。要求同 Trace/revision，关系域 present/complete、provenance=exact-binary、相同 relation_count、matched_count=0、relations=[]、returned/omitted=0、明确耗尽且无任何 partial/truncated/预算耗尽。

该证明只说明当前捕获没有这些关系，不证明完成者不存在或属于 GPU。顶层必须有同 candidate_id 的 `UnresolvedWaitChain`，同时引用两份回执并写出具体阻断与最小补证。普通 exclusive cost 不因采样出现锁就改成 metric=wait；其机制受限按原 CPU 调查结果保留，不伪造等待事件。

### 结束状态与继续拒绝的情况

以上收据完成的是“实际调查到可证明边界”。报告正文要同时显示 Unresolved、结束条件、候选/Frame/Evidence 引用和缺口。未查询、只有摘要、错误身份、未闭合分页、参数错误、来源失效或未知节点，仍然不能标为已调查完成。已有 Captured completion/匹配事件时不得改用否定分支隐藏它。

ref-based CPU/GPU tree 的 Frame/时间覆盖只能来自真实返回实体与独立 Frame/L0 锚点。请求里附带、但 API 忽略的 frame_index/frame_scope/start_ns/end_ns 不能增加代表覆盖，也不能把区间外事件放入指定帧。

实体 ref 是 opaque 值。调用栈优先把返回的完整 `callstack_ref` 交给 `callstack.resolve`，并核验返回值相同；不得把 ref 最后的十六进制片段当作十进制 ID。SiteReused 只说明注册站点，不能作为每次事件实际执行热点；事件内部热点另查严格时间/线程范围的 Sampling。

CPU/GPU 查询须绑定实际线程/Queue、完整父路径或稳定签名和所需代表帧/时间区间。只记录 frame.get、摘要、源码说明或 Sampling 而不展开候选事件树，不能关闭 CPU/GPU 候选。时间范围查询若需声称属于某 Player 帧，必须引用实际 frame.get 回执作为 frame_anchor_evidence_id，并用 frame_anchor_json_pointer 指向返回的 Frame 元数据。

报告构建器检查请求/返回 ID、Trace ID、文件 hash、查询成功和分页状态、查询对象及代表范围。错误帧、其他线程、同名不同父路径、缺失请求、截断结果以及仅有结论文字，均不能视为完成。该校验是可审计工作流约束，不宣称能抵御人为伪造全部文件。

父路径不能用同名但无关节点凑齐：链中每个 child.parent_ref 必须等于上级 ref。失败查询不能写成 Supported/Confirmed。对实际 capability/domain unavailable，可保存三次不同 MCP 请求 ID、同一 method、params、purpose 和错误代码的失败回执，设置 `outcome=unavailable`，结论只能为 `Unresolved`；参数错误、超预算和随便一种 error 都不能替代能力缺失证明。

Query 的真实错误代码为大写，失败封装可以没有 `trace`。原始失败响应必须原样保存，禁止补写 fingerprint。每条不可用调查回执额外保存：

- `unavailable_anchor`：一条完整的成功调查回执对象，含 `evidence_id`、`purpose`、`manifestation`、实体指针和必要的 Frame/L0/OriginFrame 锚点。它必须实际定位当前候选；与 Finding 中其他成功调查回执一起通过完整定向覆盖检查；不能嵌套另一个 unavailable。
- `unavailable_ref_json_pointer`：指向上述成功原始响应中已返回的真实目标引用，例如 `/data/root/source_location_ref`。失败请求的 `params.ref` 必须与该值完全相同。

例如先保存 CPU 事件树和 `frame.get`，证明事件、线程、父路径与候选代表帧匹配；再使用事件返回的 `source_location_ref` 查询 `source.lines`。失败请求通过这个成功锚点绑定 Trace fingerprint 和候选，而不是通过 `source.lines` 实际忽略的 signature/Frame/线程参数来声明范围。三个失败查询只证明该已定位目标的查询能力不可用，不补造事件或耗时。

没有完整可验证锚点时，保留错误和 EvidenceGap，候选继续待查；不得用三次失败绕过尚未完成的代表覆盖。多个代表点可分别记录锚点；anchor 与其他成功回执必须共同验证该候选要求的全部代表范围。错误码限定为 CAPABILITY_UNAVAILABLE、DOMAIN_UNAVAILABLE、NOT_CAPTURED、UNSUPPORTED_BACKEND、SOURCE_DOMAIN_INVALID、INSUFFICIENT_EVIDENCE；历史小写按同一语义识别。

阶段报告显式设置 `report_status=in_progress`，仍校验已写 finding 的数值、回执和范围，完整列出未调查候选。机器校验结果同时返回 `report_status`、`analysis_complete`、`required_pending`；阶段 `passed=true` 只表示产物合法，`analysis_complete=false`。生成器会显示未完成警告；不能把阶段报告改名后称为完整报告。默认 `complete` 对任何待查 selected 均拒绝。

完成一批调查后运行 `manage-analysis-state.py record-investigation-audit --state <state> --analysis <analysis-result> --candidates <candidate-manifest> --evidence <evidence-manifest>`，绑定三份文件路径与 SHA-256。随后重新 ingest 原候选页刷新队列。恢复和终态校验会重新检查实际回执；只填写 completed ID 不予放行。部分批次的 analysis-result 采用 in_progress，完整批次采用 complete。

质量仍为独立附录。尚未调查或有未解决的回执校验错误时，保存阶段记录；不能宣布完整报告通过。

分页完整性同时核对响应外层和 `data.page` / `data.pagination`。即使外层
`partial=false` 且没有 `next_cursor`，正文的 `has_more=true` 仍表示未耗尽；
不能用这类前缀结果证明不存在、唯一或完整覆盖。若接口漏给续页游标，保留
原请求/响应并登记工具接口缺口，改用真实支持的更窄范围补查；禁止自行
制造 cursor/offset 或将原回执改写为完整。

真实已耗尽的多页查询使用一条首页调查回执，追加
`page_evidence_ids=[首页ID, 第二页ID, ..., 末页ID]`。所有页仍是原始MCP文件，
不合成或修改响应；核验每页hash/请求ID、同一Trace及revision、同一method与
除cursor外全部参数、相同树root、逐页连续cursor，首页不得从中间开始，
末页必须明确耗尽。partial、truncated、预算耗尽、缺页、游标循环、只提交
末页均拒绝。该字段表示完整查询链，不能把分页Top视图或漏给cursor的
`has_more=true`响应伪装为全量；它不放宽路径、Frame及Queue身份门禁。

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

Query 原 P0 仅为兼容保留，当前 candidate-policy-v3 不生成它；新分析 P0 则明确指原 P1 或用户关注。按 2.2 映射，禁止改写 Query 原始级别。Capture 质量项不属于 Candidate，不消耗队列/Backlog/覆盖计数，也不要求 L6。把 `capture_quality` 原样保存到报告附录，简述受影响域、原始原因和分析限制。只有某个性能候选的调查确实需要该缺失数据时，才在该候选下登记关联的 EvidenceGap 并进行必要补查。扫描器 checksum/identity/audit 错误仍然阻断，不作为普通质量附注放行。

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

### Job 收据的帧绑定

Job 候选的代表帧是记录的 `OriginFrameId`，不是 `Player.Frame` 数组下标。
对每个代表帧保存真实 `frame.identity(frame_id)` 收据；`job.get(ref)` 或
`job.dependencies(ref)` 收据使用 `purpose=wait_completion`，通过
`job_frame_identity_evidence_id` 绑定前者，并在 `entity_json_pointers` 指向实际
Job（分别为 `/data` 或 `/data/job`）。校验 Job 类型/名称、origin ref/sequence、
相同 Trace，以及 canonical begin/end；Job 完成可以跨出来源帧，不得因此伪造帧归属。
这是“确实调查了该代表实例”的门禁，不代表该 Job 是整帧唯一根因。

方法参数不可通用套用：当前 Query 的 `zone.cpu.search` 只提供全线程时间窗检索，
额外传入 `thread_ref` 不会使它成为单线程结果。必须核对返回节点的 `thread_ref`，
再用准确 `ref` 查询树；不能将其他线程的同名 Marker 归因给该 Job。

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

1. 全部分流范围有完整登记；强制/批准深查项完成源码调查，新 P1 入队项完成证据候选报告，排除项保留原事实与理由；质量问题不计作性能调查。
2. 证据受限者有实际 MCP 查询/失败回执、最深可信结论及最小 EvidenceGap；单独一条 Backlog 原因不算调查。
3. 所有数值能回到 Candidate/Evidence。
4. 所有 Confirmed 有精确关系、匹配源码或其他独立硬证据。
5. 所有 MCP 与源码查询进入 Ledger。
6. 未调查候选和未覆盖贡献已披露。

“时间不够”“已有若干结论”或“只看了12帧”不是有效停止条件。

遇到时间/资源限制可暂停：保存剩余队列和已核查证据，生成明确未完成的阶段结果。不得将此标成完整报告或 completed_with_degradation；后者表示完成调查但源数据受限，不表示调查尚未执行。
