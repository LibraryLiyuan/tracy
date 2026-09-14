# 从候选到独立原因的深查合同

> 2.2 适用范围：先执行 [优先级与测试人员决策](priority-and-tester-review.md)。本文的原 P 级为 Query 旧编号；所有“源码必读/深查完成”要求仅适用于新 P0、原有低级别必查项及测试人员明确批准的新 P1。新 P1 入队项先完成 Query 证据核查和候选报告；未选择不算源码受阻。分流排除项保留精确 Query 事实及理由。旧流程章节仅用于其适用范围内的证据和源码合同，不能覆盖新分流规则。


本合同落实用户确认的四项要求及人工分析式“如何发现”。必须与原始收据验证同时执行。Query 1.35和候选策略不变；这里组织其结果，不重新计算排名或静默改变预算。

## 一、候选细分与分析进度

执行 `classify-candidates.py --candidates <Manifest> --output <候选视图JSON>`。把生成的candidate_views加入performance_review，并为缺少可读名称的项补写真实术语。程序根据Query事实检查分类：level区分frame_window、event、resource_operation；pattern保留frame_cost、stable_slow、sustained_pressure及其他原始表现；cost_kind区分CPU事件成本、等待候选、GPU、Job、资源、I/O等；原域、FrameSet、线程/队列和triggers保持不变。分类不是根因；只按 2.2 约定另记分析 P 级，不改变 Query 原级别或筛选阈值。

candidate_views 必须覆盖全部分流范围（含筛选排除项）及已调查候选；源码 issues 只覆盖强制或已批准深查项。未入选数据只保存数量摘要和原始机器清单，不逐条塞进待办文档。新增一个跨帧集合引用，不应自动新增一个独立问题。

结论分开记录：candidate_findings的旧状态用于具体查询结论；issues.analysis_state为complete或blocked，专门表示深查；readiness描述现在能做什么：

| readiness | 含义 |
|---|---|
| cost_located | 已定位具体开销，机制仍待查 |
| mechanism_supported | 已读源码，机制有支持，捕获版本或运行条件仍可能待核实 |
| cause_verified | 匹配源码及实际运行证据已核查，当前范围原因成立 |
| actionable | 已具备具体优化设计依据，仍需功能风险与收益验证 |
| rejected | 某项解释被排除，不代表所有成本不存在 |
| blocked | 深查受阻，只能保留阶段结果 |

“源码阅读完成”不等于“所有原因已经证明”。cause_verified/actionable须有匹配源码及runtime_verification，包含claim、reason、counterexample_review和实际evidence_refs；它们由AI复核，程序不替代因果判断。

## 二、必须保存的结构

在analysis-result.json中增加performance_review，schema_version固定为1；完整字段见schemas/performance-review.schema.json。它包含profile_evidence_id、candidate_views、evidence_labels、events、sources、issues和dedup_review；可补充relations和quality_notes。report_versions中的analysis_workflow、report_builder、template均使用2.2.0（冻结的2.1结果只按旧版复核）。

profile_evidence_id指向实际tracy_scan(profile_validate)原始请求/响应。程序核对请求ID、成功状态、valid及与Candidate Manifest一致的profile_identity，从normalized_profile.frame_budget.frame_ms读取目标帧预算；报告显示整帧耗时、目标预算与单帧超出量。它不是全量Budget Debt重统计，不能套用到GPU片段。

evidence_labels将每个用到的原始Evidence引用映射为“422帧OwnedUsage事件树”一类可读名称。机器ID不进入标题、正文、图例或链接显示文字。

events是共享事件表：event_id只作机器引用；name在机器记录保留真实返回事件名。原名不可读时另填display_name：可以使用Query实际返回的function（display_name_basis=query_function），或明确写“未解析事件”（display_name_basis=unresolved_context），以帧/线程/时间定位；禁止自行猜测函数名。thread_name填写可核查的线程/队列名称，evidence_id和pointer指向原始MCP实体。CPU/GPU区域必须具有真实ref、起止时间、duration和complete；parent_event_id必须匹配实际parent_ref、线程和包含范围。缺祖先时登记ancestry_gap，不拼接假父子链。Job/I/O/资源操作可以登记实际对象，时间不足时必须写timing_gap，不推算时长。

issues是原因条目或明确待查线索。每个强制或已批准且已调查的深查候选必须关联原因条目；新 P1 仅证据审阅项由 review_routing 登记；一个条目可关联多个候选和样本。整帧/窗口候选可关联多个不同原因，必须用candidate_splits记录candidate_id、全部issue_ids、拆分reason、evidence_refs和source_refs；覆盖只计该候选一次，所有关联原因深查完成后才将候选移出队列。必须具有title、candidate_ids、analysis_state、readiness、source_refs、source_applicability、mechanism、trigger、merge、samples、discovery_steps、counterevidence、scope、conclusion、limitations和next_actions。

## 三、源码分析不是可选段落

每项complete深查至少引用一份sources记录，且在discovery_steps中解释它如何改变判断。每份记录保存：

- source_id、可读name、repository绝对路径、仓库相对path、symbol、revision、line与end_line。
- snapshot包含Evidence根目录下的相对path及文件校验值；必须为实际源文件原字节。截取的1到80行用于清楚展示，不代表只读这些行就足够；相关调用方、条件分支或被调实现需分别登记。
- behavior解释代码执行什么；conditions解释何时执行；existing_controls说明缓存、复用、dirty/version等已有机制；runtime_support说明源码与本次录制证据的对应及未证实部分。
- evidence_refs关联实际查询证据；version_status为matched、unverified或mismatch。matched另需repository_key与build_identity_evidence_id，按原源码确认合同验证真实Trace构建身份和Git对象。

程序校验快照完整性及它与本地文件或指定Git对象的对应，核实阅读范围，并核验实际MCP引用。只写“已读源码”、一个函数名、一个路径或两个布尔值不能完成深查。即使版本不匹配也要阅读本地实现，但只能保留带限制的解释，不能据此确认捕获时根因。

找不到实现时，analysis_state=blocked，填写source_blocker.reason、实际searches和next_action。仍可保留查询事实并生成in_progress报告，但该候选不计入源码深查完成、不从必查队列移除。source.lines未嵌入源码、符号尚未定位与本地源码不可访问必须分别说明。不能因为查询三次失败就省略对本地源码的搜索。

同一源码记录可复用；每个原因用source_applicability解释其版本和条件是否适用。不得把别的问题的源码背景当成本事件实际执行分支的证明。

## 四、具体帧、调用链和“如何发现”

samples逐项列sample_id、role、选择reason、可读frame_name、frame_evidence_id、frame_pointer及event_ids。role取normal、peak、repeated、sustained、recovery或user_focus。缺正常对照写comparison_gap，不虚构正常数据。

样本kind必须尊重Query原生身份：

- frame：绑定真实frame.get，显示实际FrameSet、帧号及起止时间。
- gpu_interval：绑定真实zone.gpu.get的L0片段；验证Queue与祖先链，显示GPU片段区间，不冒充Player.Frame或整帧GPU。
- origin_frame：绑定frame.identity中的实际identity对象，核对Job的origin_frame_ref和sequence，不把OriginFrameId当Player.Frame数组索引。
- context：仅适用于实际操作快照，绑定同一查询并填写timing_gap，不能声称贡献了某帧的百分比。

每个discovery_steps条目保存question、action、observation、judgment、sample_ids、evidence_refs、source_refs。按“为什么查这里 → 实际查到什么 → 因此支持/排除什么、下一步为何转向”组织；历史顺序不确定时在说明中注明按已有证据重建，不编造操作流水。

报告由Query原值展示完整Event耗时、自身时间和运行时间。占帧使用事件与帧的交集作分子、该帧时长作分母；占父事件使用实际直接父事件完整时长。分母缺失则不显示百分比。父子或并发时间不相加，持续时间不自动等于CPU忙碌，采样比例不当精确耗时，耗时占比不当可兑现收益。

调用次数、分配量或更多计时可在issue.measurements登记name、quantity、scope、interpretation、evidence_id、pointer、sample_ids。quantity限定duration_ns、count、bytes、sampling_percent；数值只从原始Query字段取，不填写自算value。程序检查字段的计量语义；暂不支持的字段应补充明确合同，不能冒用另一种quantity。无调用计数时明确缺失，不以候选数或采样命中数推算调用次数。

跨线程依赖另放relations：指向真实关系回执，核对source_ref、target_ref、relation和exact证据；from_event_id、to_event_id只关联已有实体。name和meaning解释真实依赖，issue.relation_refs列所用关系。调用父子链与等待完成链分开画清楚。

counterevidence必须写具体observation、对解释的effect、evidence_refs及source_refs。源码已有缓存、正常帧同函数很快、恢复期该Event接近零但整帧仍慢等都应影响结论，不能写“无反证”代替检查。

## 五、原因归并与最终复核

每个条目的merge说明basis和reason。single_candidate用于单候选；same_event必须由成员各自核验的收据证明共同实际事件；verified_mechanism用于不同实例已核实同因，须有匹配源码、运行验证和merge.evidence_refs。仅名称相同或文字相似不构成同因证明。

同一实体不能在events里以不同代号重复注册。同事件、同源码位置或同名事件关联到不同条目时，dedup_review必须记录两条issue_ids、不同原因的reason、evidence_refs和source_refs。相同机制和触发条件不能复制成两条；共享上游原因时应合并并保留下游不同表现。人工复核必须检查归并理由是否确实得到双方实例支持，程序只能检查结构与引用。

最终复核逐条回答：代码已经有什么优化，本次为什么仍产生工作？哪些运行证据表明执行了所述条件？正常/异常是否可比？是否只解释了局部成本？同因是否重复、不同因是否误合并？尚未回答的部分写具体限制和下一步，不能用统一Supported或Unresolved覆盖。

## 六、输出和恢复

正常validate_analysis_result.py、build_report.py和状态审计都执行新合同。没有performance_review的旧材料必须补查或作为明确历史回归；--legacy-regression永远不能产生当前analysis_complete=true，禁止用于正式流程。

主报告每个原因只解释一次；专项、发现过程索引和人工回查指南链接到该条。Markdown及可选HTML均使用可读名称，原始JSON与源码快照独立打包。每份quality_notes以可读description保留对应capture_quality项，不能隐藏数据质量限制。

报告保存至空输出目录并用打包后的材料再次校验。状态审计只把analysis_state=complete条目的成员写入completed；已查询但源码受阻的成员保留在队列。待办只列必查/已选问题，不把未入选数据库逐行打印。机器覆盖数、独立事件数、整理后条目数分别列出，最后一种包含待查线索，不冒充确认缺陷数。

原始Query比例保持原值；报告另行计算的单实例交集占比属于展示字段，不覆盖原始比例或重算全量统计。一候选多原因的拆分不能用于重复累计整帧影响。
