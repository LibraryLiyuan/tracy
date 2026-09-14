# 优先级与测试人员决策（2.2）

## 编号约定与兼容边界

Query 1.35.0 / candidate-policy-v3 仍输出旧编号。本版 SKILL 不修改 Query 二进制、Profile 阈值或 Candidate Manifest。机器数据保留 `query_priority`，另记 `analysis_priority`，人类文档必须标明“Query 原级别 / 分析级别”，禁止把转换后的值回写 Query priority。

| Query 原级别或触发 | 新分析级别 | 流程 |
|---|---|---|
| 原 P0 / quality | 不分性能级别 | 采集与分析问题汇总；当前 Query 不生成原 P0，出现时核实旧协议，不当性能候选 |
| 原 P1 | P0 | 强制 AI 深度分析和源码阅读 |
| user_focus（原 P2） | P0 | 同上；用户要求查不表示已确认缺陷 |
| 原 P2 | P1 | 先筛选、核查 Query 证据、生成源码深查候选报告，测试人员选择后源码深查 |
| 原 P3 | P2 | 保留原筛选语义；已 selected 的原有深查要求不因本次分流而放宽 |
| 原 P4 | P3 | 同上 |

稳定偏慢窗口与持续压力窗口不升级：原 P2 → 新 P1。不要将窗口表现继承成函数缺陷。原 P1 的整帧/窗口仍是新 P0。若同项还有 user_focus，则按新 P0。`capture_quality` 没有性能等级，不占名额。

`review_routing.query_priority_scheme` 固定为 `query-v3-P1-P4`，policy 为 `renumbered-p0-deep-p1-tester-v1`。将来 Query 改编号必须更新适配并验证，不能按相同字母重复递推。旧分析结果保留 2.1 版本身份，不原地迁移。

## 筛选与证据分析

使用 `prepare-review-routing.py --candidates candidate-manifest.json --output review-routing-draft.json` 生成草稿；程序完整验证候选文件，且拒绝覆盖已有决策。草稿未填写的字段不能通过完成校验。把填写后的对象放入 `analysis-result.json.review_routing`。

每个 selected 或新 P0 候选必须有且只有一条记录。Query 未选中的普通候选仍保留在 Backlog，不展开全部百万级记录。用户关注即使 selected=false 也必须进入新 P0；新 P0 不接受排除、暂缓或拒绝深查。

新 P1 不按前 N 项、字符数或总文章长度截断。依据已保存的 Query 事实逐项筛选：显著单次卡顿、反复成本及关键路径信号应保留；重复观察可以关联，正常帧的必要成本可以降低建议深查程度。没有证据不等于无问题；未知影响仍可进入决策报告。

- `route=review`：进入证据分析与决策队列，必须完成原有 candidate_findings/定向回执合同；其中 source_analysis 明确“未选择源码深查”，不伪造已读实现。
- `route=filtered`：尚未进入源码决策队列，decision=not_applicable。填写 screening_reason，完整复制本候选 trigger_evidence 到 screening_facts，保留原候选。它不是深查完成。不得排除仅因名称陌生、源码缺失或达到前 N 上限的候选。
- `route=mandatory_deep`：新 P0 或原有低级别已选项，decision=not_applicable，执行完整源码合同。

## 候选报告：一眼看懂影响

输出 `P1-Source-Review-Candidates.md`。每项先用一句话说明真实操作、具体帧/原生区间、发生表现；首页表格显示真实名称、Query 原级别/分析级别、影响程度、现象和测试人员决定。标题使用“候选/观察”，不能统一称为已确认问题。

每个 review 记录以下字段（文字都要可读且有具体证据，不用内部身份填充）：

| 字段 | 内容 |
|---|---|
| phenomenon / scope | 现象、FrameSet 帧号或 GPU 原生区间、线程/队列 |
| selection_reason | Query 触发规则、原阈值和实际值；同时命中多规则时保留各项 |
| frequency | 发生帧数、达到阈值帧数及完整帧分母；单次/重复/持续形态 |
| slow_frame_relation | 正常与慢帧对照、实际父子或等待关系；关键路径证实或仅同时出现 |
| overlap | 同一事件、父子、跨帧、线程/CPU-GPU 并发是否重叠；不相加 |
| impact / impact_reason | high/moderate/low/unknown 及证据理由；没有通用硬阈值，不等同 P 级 |
| impact_evidence | critical_path/correlation_only/not_established；无关系证据不能选 critical_path |
| uncertainty | 原因、运行分支、必要性、可优化性分别说明；不得用一个 Supported 掩盖 |
| recommendation / next_evidence | 是否建议源码深查、具体要查什么、查清后支持什么决定 |

metrics 必须登记 peak_cost、typical_cost、self_cost、inclusive_cost、calls、affected_frames、complete_frames、frame_budget。每项要么 `status=available` 并给出 Query 原值 value、unit、scope、evidence_id、pointer，要么 `status=unavailable` 并写具体缺口原因。默认 pointer 指向已核验 MCP structuredContent 中的原字段；origin=candidate 时指向本候选的原始数字字段（例如 /trigger_evidence/0/observed_value），适用于已有触发数值和定向查询受限的情况；不能把作者编写的数字当证据。不是所有域都提供这些指标，不适用时如实说明，禁止补造百分位。

成本口径明确区分完整事件、帧内交集、自身/包含子调用、运行/等待。代表实例的时长不能冒充路径峰值，少量样本不能冒充全量发生率。所有占比有分母，不把耗时占比当收益。至少有一项真实 Query 数值；若定向能力受阻，可用该候选自身已保存的 Query 触发数值并登记定向缺口，不可把全部指标写成未知。

示例：单帧累计 1.2 ms 只说明达到了绝对成本入口。若整帧未超预算且无关键路径影响证据，应展示“正常帧中的成本观察”，不能直接写“导致卡顿”。父路径 2 ms 包含子路径 1.2 ms 时不能合计成 3.2 ms。

## 测试人员决定与恢复

review 项 decision 默认 pending。approved/declined/deferred 均必须引用 `tester-decisions/*.json` 中的决定记录，通过 decision_record={path,sha256} 绑定。记录包含 candidate_id、candidate_content_sha256、decision、origin（user_message/tester_record）、actor、at、reason、source_reference。source_reference 应定位实际用户消息或测试记录；AI 推荐不算批准，等待时间不算批准。

决定必须由真实测试人员提供。校验器能检查身份、文件与字段，不能证明文件作者确为测试人员；代理仍必须遵守真实授权。一个决定文件只绑定一个候选；批量选择逐项保存，保留原决定历史。新 Trace/Profile/候选身份变化后不能沿用原批准。

approved 后该项进入源码队列。使用已有 Query 回执继续，必要时补查；仍未读源码或源码不可得时为深查待完成/受阻。pending/declined/deferred 不进入源码队列，也不声称源码完成；在独立决策集合保留。

完成口径：Query 调查覆盖、源码深查覆盖、待测试人员决定分别计数。`analysis_complete` 仅指本轮规定范围；`required_pending` 为强制/已批准源码项缺口。并列显示 tester_decision_pending、source_scope_complete、all_selected_source_complete。新 P1 决策报告已完成但未选择，可交付本轮报告；不能称“所有候选已深查”。`record-investigation-audit` 校验后重建 query_reviewed_ids、filtered_ids、tester_pending_ids 和源码队列；恢复必须重新核验决定文件和原始回执。

## 采集与分析问题汇总

`analysis.process_issues` 为数组；无额外已知问题时可为空，不能省略字段。每项包含 name、category（capture/query/source/ai_analysis）、fact、impact_scope、consequence、next_action、evidence_refs。只写已观察到的缺失、错误或执行遗漏，不把尚未阅读源码说成源码不存在。证据引用可包含真实失败回执，不能将失败写成成功。

输出 `Capture-and-Analysis-Issues.md`，汇集 capture_quality 与 process_issues，关联证据缺口和受影响分析范围。无性能优先级、无优化收益计数。原始 capture_quality 原样保存在机器文件；人类文档用可读名称与解释。
