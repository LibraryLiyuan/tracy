---
name: diagnose-jn-unity-tracy
description: Use when diagnosing a single existing JN Unity Tracy capture with Query-native deterministic scanning and an auditable performance report.
metadata:
  author: JN Game
  version: 2.3.0
---

# JN Unity × Tracy 单 Trace 深度诊断

## 原生视图与交互回查（2.3.0）

生成新报告前读取 [native-profiler-views.md](references/native-profiler-views.md)。每个完成的深度观察及新 P1 决策样本登记原生视图/回查入口或明确展示缺口；截图与链接共享经核验的定位身份，分别验收数据定位、原生图片及真实交互。正常分析仅调用已发布、验证过的工具，不临时编译或修补 Profiler。

本次新增执行规范，现有生成器和校验器尚未实现通用 Profiler 接口或展示门禁；必须另存 view-manifest.json 与 presentation-validation.json，不得把旧 passed 标志当作原生视图验收。工具能力缺失时保存合法分析结果并单独披露展示 pending/degraded，不能把静态图或冻结网页标为可交互。

SKILL 发布版本为 2.3.0；Query 1.35.0、candidate-policy-v3、Schema 4、performance_review/review_routing schema 1 与 report_versions=2.2.0 均保持兼容，新增展示记录独立保存，不向旧严格 schema 注入未支持字段。

## 继续沿用的报告展示规则（2.2.1）

以下排序与正文精简规则继续适用；展示升级不改变优先级、Query 统计或源码深查范围。

- 主报告按主要样本的真实帧号数值升序排列，先 `Frames`，再 `Player.Frame`、`Render.Frame`，其他 FrameSet 独立分组。主要样本取首个 `peak` 帧样本，没有时取首个帧样本；不能用较早的正常对照帧替代主要帧。条目作者应把目标样本放在前面。同号条目按实际区间及稳定身份排序；无普通帧号的原生/GPU区间放后面，不伪造帧号。
- 新 P1 候选报告按同一 FrameSet 的窗口起始帧或最早代表帧升序；原生 Job 与 GPU 单列，不把原生身份当普通帧号。排序后统一生成目录、条目编号、专项和人工回查链接，不按字符串排序，也不修改原候选优先级与数值。
- 每项深度观察的阅读正文不再输出“如何发现”和逐步骤叙述。仍展示结论、具体样本/调用链、数值、源码、反例与下一步。`discovery_steps` 和 Analysis Trail 继续保存在机器 JSON 并校验，不因精简正文而省略调查。
- 本规则适用于新生成的 Markdown 及可选 HTML；分册从同一排序结果生成。不要原地改写已冻结报告。

使用 Query 1.35 对一个已存在的 `.tracy` 做确定性全量扫描，再由 AI 核查证据、按分析优先级分流：新 P0 强制源码深查，新 P1 先生成测试人员决策报告。本 dev 基线发行版仅支持普通 `.tracy`，不包含 N30 Session Store。Query 拥有全部统计数值；AI 只负责证据驱动的解释、假设验证、源码核查和优化建议。

## 输入与边界

开始前必须具备：

- 一个 Trace 输入：已完成转换的普通 `.tracy` 文件。
- `config/JNTracy.AnalysisProfile.yaml`。
- 一个 Query schema 精确为 `1.35.0` 的常驻 Tracy Query MCP。
- 一个位于用户授权目录内的独立分析输出目录。

只分析一个 Trace。不做多运行 A/B、源码自动修改、自动提交或工具链性能验收。若输入是 `.tracy-stream`、用户要求开始/停止录制，先按 [capture-workflow.md](references/capture-workflow.md) 完成独立路由；获得可分析输入后再开始本流程。

不得猜测 Query 路径、Trace 身份、源码 revision 或缺失 capability。Profile 缺失/不合法、Query schema 不等于 `1.35.0`、输入不受支持或尚未 ready，均停止新分析并给出明确阻断原因。

## 必须读取的参考

按以下顺序完整读取：

1. [query-native-scan-workflow.md](references/query-native-scan-workflow.md)：MCP、Scan 和恢复。
2. [candidate-investigation.md](references/candidate-investigation.md)：候选覆盖和逐项深查。
3. [domain-playbooks.md](references/domain-playbooks.md)：CPU、GPU、Job、Memory、I/O、Sampling 专项。
4. [evidence-and-source.md](references/evidence-and-source.md)：调用栈、源码和 relation 可信度。
5. [report-contract.md](references/report-contract.md)：结果、验证和报告产物。
6. [priority-and-tester-review.md](references/priority-and-tester-review.md)：2.2 优先级映射、筛选、测试人员决策和完成口径。
7. [deep-analysis-and-report.md](references/deep-analysis-and-report.md)：被要求或批准深查的候选，执行源码、逐帧发现过程和原因归并合同。

涉及 GPU 资源时再读取 [gpu-resource-catalog.md](references/gpu-resource-catalog.md)。参考文件不再递归路由。

## 唯一配置权威

`config/JNTracy.AnalysisProfile.yaml` 是 Query 预算、Marker 规则、原始候选策略、用户关注和资源限制的唯一运行时权威。使用安全 YAML loader 读取，然后通过同一个常驻 MCP 的 `tracy_scan(operation="profile_validate")` 交给 Query 严格校验和规范化。

把以下内容落盘：

- 原始 YAML SHA-256。
- Query 返回的 normalized Profile。
- `profile_identity`。
- Profile validation MCP 请求与响应。

旧 JSON 中的预算和筛选设置只属于历史回归资产，不得参与 2.0 统计。`config/local-profile.json` 中本机工具路径和 `source_roots` 仍用于启动工具和限定授权源码目录；它们不覆盖 YAML 的分析配置。Python 只可安全解析 YAML、维护状态、核验保存的证据和渲染报告，不得读取 Trace 或重新统计性能数据。

正常入口只接受 `candidate-policy-v3`；缺失、旧版或未知策略都必须报错，不能借版本差异跳过收据审计。恢复已完成调查和写入完成状态也必须重新验证证据。

## Schema 4 状态机

从 `assets/analysis-state-template.json` 创建 `analysis-state.json`。只恢复 Schema 4；旧 Schema 2/3 保持只读，不迁移。

```text
created
→ profile_validated
→ mcp_ready
→ trace_ready
→ scan_running
→ scan_cancelled_resumable / scan_complete / scan_failed
→ candidate_manifest_pending
→ investigating_candidates
→ assembling_report
→ validating_report
→ completed / completed_with_degradation
```

每个状态变化必须保存原因和 Evidence 路径。使用 `scripts/manage-analysis-state.py` 做状态、候选队列和数值一致性校验；该脚本不读取 Trace。

恢复规则：

- `CancelledResumable`：调用 `tracy_scan(operation="resume")`，不得重新创建分析任务。
- Query 进程重启：以已保存 `scan_id` 调用 `status`；Query 会恢复持久状态。
- Scan `Complete`：复用相同 Aggregate/Candidate identity，不重新扫描。
- 只有 Profile 变化：Query 复用 Neutral Aggregate，仅重算 Policy。
- `Failed`、identity mismatch、checksum 错误：停止，不以空结果继续。

## 固定执行流程

1. 创建 Schema 4 状态；验证 YAML 存在且可安全解析。
2. 启动或连接唯一常驻 Query MCP；记录 EXE SHA-256、PID/start identity、initialize 和 tools/list。
3. 调用 `tracy_describe`，确认 Query schema 为 `1.35.0`，且 `tracy_scan` 包含全部 12 个 operation。
4. `tracy_trace_open` 打开输入；轮询到 ready，保存强身份、完整性、capability 和 domain quality。
5. 通过 `tracy_scan(profile_validate)` 规范化 Profile，再调用 `tracy_scan(start)`。
6. 轮询 `tracy_scan(status)`。可取消状态使用 `cancel`；恢复使用 `resume`。不以固定等待时间推断完成。
7. Complete 后读取 `summary` 和 `quality`，再用 `candidates` 完整分页 Candidate Manifest。响应必须先落盘，不能把全量 raw event 倾倒进模型上下文。
8. 将 `summary/quality.capture_quality` 原样保存为报告附录；不进入候选或调查队列。使用 2.2 映射生成分析优先级，原 Query priority/selected 不改写；质量问题进入独立汇总。对所有 selected 或映射后新 P0 项完成分流登记，不只取前 N 项。
9. 用 `scripts/classify-candidates.py` 从原始Candidate Manifest生成多维候选视图；保留 query_priority、触发理由和原数值，另写 analysis_priority；映射只改变流程分级，不重排成本或从名称推断原因。用 prepare-review-routing.py 生成 review_routing 草稿并由 AI 完成筛选说明。
10. 对需要证据分析的候选调用 `candidate_get`、`representative_frames` 并定位真实事件和等待关系。新 P0 强制源码深查；新 P1 筛选入队后先解释 Query 情况、影响程度和不确定性，生成源码深查候选报告，测试人员明确选择后才深查。新 P2/P3 保留原 Query 入选语义，已 selected 的源码深查要求不在本次豁免。
11. 填写 `review_routing` 与独立问题汇总；对强制或已批准源码深查项填写 `performance_review`：候选视图、共享事件表、源码阅读、具体样本、逐步发现过程、反例及原因条目。按实际同事件或已核实机制归并；不同原因保留明确区分依据。
12. 完成当前要求的源码深查和新 P1 决策报告后，复核原因归并，运行数值校验、报告验证和生成器。正文按原因组织，专项仅引用同一正文条目；未入选候选不展开为用户待办。
13. 按 native-profiler-views.md 完成视图定位、原生导图、交互检查及独立展示状态；失败时保留合法分析并交付明确展示缺口。
14. 分析报告校验及展示状态记录完成后调用 `tracy_scan(close)` 与 `tracy_trace_close`，保存最终状态。

每一步的退出条件和失败语义见 [query-native-scan-workflow.md](references/query-native-scan-workflow.md)。

## 调查覆盖规则

Query Candidate Manifest 是候选和数值事实的权威：

- 原 Query P1 → 新分析 P0；原 P2 → 新 P1；原 P3 → 新 P2；原 P4 → 新 P3。用户关注从原 P2 提升到新 P0。两个窗口类别仍属于原 P2，即新 P1，不提升为强制级。
- 原 P0/quality 不映射为新性能 P0。当前 Query 不生成原 P0；若出现，记录在采集与分析问题汇总并核实旧协议，不当作性能候选。拒绝未知编号约定，禁止重复递推。
- 所有新 P0 强制 AI 及源码深查。新 P1 必须逐项筛选；入队后必须做 Query 证据分析与可读决策报告，待测试人员选择；未选择不是源码受阻，也不能伪造深查完成。
- `capture_quality`：只作为附录简述问题、受影响域和分析限制，不分配性能优先级，不占候选/调查名额，不默认深查。仅当具体性能候选依赖缺失数据时，在该候选下关联 EvidenceGap 并定向补查。
- 旧 Manifest 含 `domain=quality` 或 `trigger=quality` 时不得沿用；使用 `candidate-policy-v3` 重新生成，不能把质量项降成 P1/P2 或静默改写旧证据。
- 扫描器自身 checksum、identity、计数审计失败仍然阻止发布/使用候选；与源 Capture 已声明的域缺失严格区分。
- 所有 `selected=true`：必须有分流记录；新 P1 的筛选排除须保留原始触发事实和具体原因，不能凭 Top N 或文章长度截断。筛选入队项的 Query 证据必须核查；只有明确批准才进入源码深查。
- 其余：保留在 Investigation Backlog，并披露未调查贡献和原因。
- 已完成 candidate ID 不得在恢复时重复调查。
- Query 提供的 percentile、inclusive/exclusive、Debt、次数、原始比例、峰值和容量值必须逐字段引用，AI 不得重算或改写。2.1允许的单实例展示占比另列，不能覆盖Query原始统计。
- partial、truncated、分页未完成、domain invalid 或第三次查询失败必须成为 EvidenceGap，不能解释为空。

候选并不自动等于根因。最长 Zone、Top Marker、Sampling 热点、显存峰值只建立假设；根因必须沿真实线程、Job、Fence、Submission、GPU Pass、Allocation 或 I/O relation 验证。

## MCP 与源码账本

每个逻辑请求最多三次：通常采用原范围、缩小/分页、拆分实体。能力缺失分支按 candidate-investigation 的同参数三次合同处理，不改变目标。全部请求记录 method、canonical params、attempt、时间、响应路径/SHA、cursor、partial/truncated、Trace/scan identity 和质量。

每条深查必须记录实际源码阅读，不能只在Confirmed时才阅读。源码证据记录仓库、revision、文件、行、符号、快照、实现解释、触发条件、已有优化及与Trace证据的对应。revision 不匹配时只能形成 Hypothesis，不能形成 Confirmed 源码结论。

所有显著结论都要同时能回到：

```text
Candidate trigger
→ Query numeric facts
→ representative frame / event refs
→ directed MCP evidence
→ typed relation or explicit EvidenceGap
→ source reading evidence（每项深查必需）
→ conclusion and retest metric
```

## 禁止事项

- Python、PowerShell 或 AI 扫描全量 Trace。
- AI 计算 percentile、exclusive time、Top 排名或候选显著性。
- 用固定 12 帧代替全量扫描。
- 启动一次性 Query CLI 绕过常驻 MCP。
- 把父子 Zone、不同线程、不同 Queue 或 CPU/GPU 异步时间直接相加。
- 把等待墙钟写成 CPU 执行时间，把 working set 写成物理显存。
- 把源码猜测、名称相似或时序接近写成已证实 relation。
- 把单 Trace 写成 Tracy 总体 A/B 开销；固定使用 `TotalCaptureOverhead = NotMeasuredSingleTrace`。


## 2.2 分流与深查门禁

`report_versions` 的 analysis_workflow/report_builder/template 均写 `2.2.0`；`performance_review` 与 `review_routing` 均使用 schema 1。Query 仍是 1.35.0/candidate-policy-v3，按旧编号读取；未修改 Query 二进制。没有新合同的旧结果在普通报告入口失败；`--legacy-regression` 只供历史测试，不能形成当前分析完成声明，禁止用于正式分析或绕过缺源码。

- **候选分类**：观察层次、性能表现、成本类型、原始触发理由和范围分别记录。CPU自身时间不自动等于CPU执行时间。
- **源码必读**：每个深查完成条目必须引用已核验的实际源码快照和具体阅读解释。可复用同一源码记录，但逐原因核对适用条件。源码不可得只交阶段结果并保留实际搜索与下一步，不能将深查标为完成。
- **如何发现（机器记录）**：在 discovery_steps / Analysis Trail 保存真实选样、父子链、数值、正常异常差异、源码、反例及判断变化；正文不输出逐步“如何发现”，只呈现核查所需样本、证据与结论。缺数据明确留作缺口，不编造查询顺序。
- **占比**：只从已经保存并核验的Query计时做展示换算；明确分母，跨帧只用交集计算该帧贡献，完整事件另列。父子和并发时间不相加，不把采样比例写成耗时，不把占比写成优化收益。
- **原因去重**：同事件复用一个实体；同因实例归成一个条目。一整帧/窗口包含多个不同原因时登记candidate_splits，保留多对多关系，候选覆盖只计一次。跨实例机制归并须有匹配源码和运行核验。相同事件、源码位置或同名事件分成多个原因时必须记录区别和证据；不得仅按文字相似自动合并。
- **全文可读**：所有Markdown、HTML和图表使用实际事件、业务术语和可读证据名称。无名称时不退回哈希/内部ID；程序会拒绝不可读展示。原始身份仅在机器JSON、原始请求和源码快照中保留。

`candidate_findings` 仍登记查询覆盖；`performance_review.issues.analysis_state` 单独表示源码深查是否完成。强制或已批准候选源码受阻时仍留在深查队列；新 P1 尚未选择时进入独立 tester_pending_ids，不记作深查完成或失败。原有verified_absent等分支只关闭对应证据查询，不能代替源码深查。

完成标志要求所有强制及测试人员已批准候选关联到源码深查完成条目，同时所有新 P1 入队项已有经核验的候选报告。analysis_complete 只表示本轮规定范围完成；必须并列披露 tester_decision_pending、source_scope_complete 和 all_selected_source_complete，不能声称全部候选均已深查。原因是否已核实用readiness另表述；深查完成不等于可优化收益已证明。报告字段校验不能证明文字因果推理正确，发布前必须复核源码解释、反例和归并依据。

## 报告完成门禁

AI 只提交结构化 Analysis Result、Evidence Manifest、Analysis State 和文字分析字段。Query 数值必须原样引用 Candidate/Evidence。运行：

```powershell
python scripts/prepare-review-routing.py `
  --candidates candidate-manifest.json `
  --output review-routing-draft.json

python scripts/classify-candidates.py `
  --candidates candidate-manifest.json `
  --output candidate-views.json

python scripts/manage-analysis-state.py validate-numerics `
  --analysis analysis-result.json `
  --candidate-manifest candidate-manifest.json

python scripts/validate_analysis_result.py `
  --analysis analysis-result.json `
  --evidence evidence-manifest.json `
  --candidates candidate-manifest.json

python scripts/build_report.py `
  --analysis analysis-result.json `
  --evidence evidence-manifest.json `
  --candidates candidate-manifest.json `
  --output AnalysisReport `
  --deterministic `
  --export-static
```

维护或发布 SKILL 时运行相关 Python/PowerShell 回归及 `python scripts/lint-skill.py`；每次分析只运行本轮产物校验，不重复整个 SKILL 测试套件。展示验收另按 native-profiler-views.md 保存状态，现有脚本 passed 不覆盖它。报告包含：强制/已批准深查结果、新 P1 源码深查候选报告、筛选排除记录、未选择/暂缓/不深查状态、Backlog、证据缺口，以及独立的 Tracy 数据收集与 AI 性能分析问题汇总。

发布同时检查 passed、report_status、analysis_complete、required_pending；required_pending 只计算本轮应深查项。新 P1 已完成证据报告但尚待选择，可以交付完整的本轮规定范围报告，并明确尚有多少项未源码深查。已批准项或强制项尚未完成时只能交付 in_progress。状态恢复和批准记录必须绑定当前候选身份及原始证据，不能复用旧清单的批准。完成后用 record-investigation-audit 重建源码队列、证据已核查集合和待选择集合。

默认不修改业务源码、不发送外部消息、不自动替测试人员批准。2.1 冻结结果只按旧规则复核；切换到 2.2 要在新输出目录显式生成新分流记录，不能将旧完成标志当新流程已完成。


以下是证据查询停止条件，不免除2.1源码深查门禁。具体 CPU 事件在代表帧确实未记录，或 Native Wait 的完成端未被捕获时，使用 [candidate-investigation.md](references/candidate-investigation.md) 的 `verified_not_recorded` / `completion_not_recorded` 收据合同。只有实际范围、完整查询和候选关联缺口全部核验成功，才以 `Unresolved` 结束调查；它不表示根因已确认或成本为零。其他代表仍必须调查，partial/漏页/错误查询不得借此关闭。
