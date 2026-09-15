# 固定报告合同

> 2.3 原生配图与交互回查按 [native-profiler-views.md](native-profiler-views.md) 执行。展示结果独立验收；本文旧脚本不会自动校验新展示合同，不能仅凭 report-validation.passed 宣称全部配图与交互已完成。

> 2.2 适用范围：先执行 [优先级与测试人员决策](priority-and-tester-review.md)。本文的原 P 级为 Query 旧编号；所有“源码必读/深查完成”要求仅适用于新 P0、原有低级别必查项及测试人员明确批准的新 P1。新 P1 入队项先完成 Query 证据核查和候选报告；未选择不算源码受阻。分流排除项保留精确 Query 事实及理由。旧流程章节仅用于其适用范围内的证据和源码合同，不能覆盖新分流规则。


> 2.1流程补充：必须同时执行 deep-analysis-and-report.md。下文保留的Candidate Finding与收据完成条件仅证明查询覆盖；所有深查完成条目都必须读实际源码。所有报告正文改用可读名称；本文出现的candidate_id、ref、SHA及Evidence字段名属于机器合同，不能直接作为报告展示内容。源码不可读时保留阶段结果，不以Unresolved自动完成整项深查。


## 1. 权威数据流

```text
Query 1.35 Aggregate Store（中立事实）
→ Candidate Manifest（确定性候选）
→ 常驻 MCP 定向查询 + Source 检查（解释证据）
→ evidence-manifest.json + analysis-result.json
→ validate_analysis_result.py
→ build_report.py
→ 主报告 + 9 份专项报告 + 经验证的原生 PNG/交互入口或展示缺口（辅助 SVG/MCP PNG 分别标明来源）
```

AI 不重新扫描 Trace、不重算 Query 数值，也不手写最终布局。Query Aggregate/Candidate、MCP 定向证据与 Source 证据必须分开登记。Markdown 是权威结论；原生 Profiler PNG 按 2.3 合同生成，辅助 SVG 与 MCP PNG 不替代原生视图。静态 HTML 报告默认关闭，由用户明确要求后生成；独立 Profiler 运行时入口不受静态报告 --html 开关控制，其能力与可达性必须实测。

大型 Candidate Manifest 使用小型 JSON 描述文件及同目录 `candidates.ndjson`（或 `candidate-records.ndjson`）。以 `candidate_records={encoding:"ndjson-v1",path,sha256,count}` 替代内嵌 `candidates`，二者不能同时存在；这是保存已导出 Query 候选的包装，不改变 Query policy/schema 或候选内容。报告、数值和状态审计通过 `candidate_manifest.py` 逐条读取，读到 EOF 才完成计数/SHA 校验；错误阻止发布。描述文件和 NDJSON 原字节复制进报告，保留身份及可移植性。

单描述文件/单行最大 16 MiB；不安全路径、保留输出名冲突、重复 ID、缺失行、尾部 hash/count 错误均拒绝。此改动避免保留全量候选 payload，但所有 ID、调查状态和 Backlog 仍占内存；不得声称整个报告流程已经常数内存化。完整报告须覆盖全部 selected 的分流；新 P1 入队项须有实际证据报告，源码完成范围由新 P0/原有低级别必查项和测试人员批准项确定。禁止只取前 N 项规避门禁。

## 2. Analysis Result

Schema 位于 `schemas/analysis-result.schema.json`。每个显著 Signature 必须包含稳定键、显著性原因、影响范围、频率、Budget Debt、Primary Limiter、代表实例、因果链、假设账本、深度、Evidence、provenance、支持/不支持结论、优化方向、功能风险、理论收益上限、复测指标和置信度。

每个 Signature 都必须有独立 Evidence Card。`deepest_level != L6` 时必须携带最小 EvidenceGap，否则校验失败。

Query-native Workflow 2.1 还必须包含 `performance_review`（schema见performance-review.schema.json）以及：

- `query_scan`：Scan、Aggregate、Candidate Manifest、Profile SHA-256 与覆盖率。
- `candidate_findings`：每个已调查 Candidate 的结论与完整 Analysis Trail。
- `investigation_backlog`：未完成 Candidate、原因与下一步。

每个 Candidate Finding 必须逐字携带 Query 的数值事实，不允许 AI 改写、四舍五入或重新计算。代表帧必须来自 Candidate Manifest，并说明 `selection_reason`。Analysis Trail 固定包含 discovery、statistics、representative selection、attribution、hypothesis、source inspection、counterevidence 和 conclusion。

`Confirmed` 的 `confirmation_basis` 必须声明至少一种已核实依据：`typed_relation`、`source_revision_match` 或 `independent_evidence`。源码确认按 evidence-and-source 的机器证据合同办理。单 Trace 报告固定不得宣称总体 A/B Tracy 开销。

因果链须引用该 Signature 自身引用的 Evidence。每条 `relation_ref` 的 `from_ref`、`to_ref`、`relation` 必须与原始关系的真实端点和 `kind` 一致；反向连接、替换关系类型或同一引用出现互相矛盾的关系，都必须失败。端点分别存在不等于它们之间存在关系。

`independent_evidence` 依据还须填写同名数组，每项为 `{evidence_id,channel,supports}`：引用真实成功回执，`channel` 为原始查询 method 的首段（如 `zone`、`sample`、`job`），`supports` 用具体文字说明它支持同一结论的哪一步。至少两项来自不同测量通道。相同 Evidence 引用、同一请求/结果的别名、同一查询的分页、同一通道换参数，均不能满足这项确认依据。不要人为改写 channel。

不同通道只是可机验的必要条件。AI 仍须解释它们为什么共同支持所述原因，并审查共同来源、派生关系和反证；“两条查询成功”不自动证明独立性或根因。机器验证通过表示保存的材料满足证据合同，不表示每句性能解释都已被程序证明。

报告必须打包原始 MCP 请求、响应以及已引用的源码快照。生成后应能直接用报告目录内的三个 JSON 文件再次运行 `validate_analysis_result.py`。源码版本复核仍需要对应的授权本地 Git 仓库。

## 3. Evidence Manifest

Schema 位于 `schemas/evidence-manifest.schema.json`。每条 Evidence 记录 Trace identity、MCP method/params、request/response path 与 SHA-256、frame/time/thread/queue/entity refs、partial/truncated/cursor、attempt、quality 和 data path。

跨 Trace 引用、SHA 不符、数据路径逃逸、时间反转、Zone 越界、父子错误或未披露截断必须失败。Renderer 只从 Evidence 时间和 refs 计算坐标与关系。

## 4. 输出目录

```text
AnalysisReport/
├─ Performance-Analysis-Report.md
├─ Analysis-Process-and-Query-Audit.md
├─ Evidence-Index.md
├─ 01-Trace-Quality-and-Scan-Coverage.md
├─ 02-Frame-and-CPU-Analysis.md
├─ 03-GPU-and-Render-Analysis.md
├─ 04-Job-and-Scheduling-Analysis.md
├─ 05-Memory-and-GPU-Resource-Analysis.md
├─ 06-IO-Sampling-and-System-Analysis.md
├─ 07-Tracy-Telemetry-Overhead.md
├─ 08-Profiler-Manual-Verification-Guide.md
├─ 09-Investigation-Backlog-and-Evidence-Gaps.md
├─ analysis-result.json
├─ candidate-manifest.json
├─ evidence-manifest.json
├─ manifest.json
├─ report-validation.json
├─ evidence-data/
└─ visuals/
   └─ assets/
      ├─ data/
      ├─ frame-images/
      ├─ static-previews/
      └─ exports/
```

当且仅当用户明确要求并传入 `--html` 时，额外生成 `visuals/index.html`；`--html --standalone` 再生成 `standalone.html`。主 HTML 通过本地 `<script src>` 加载安全数据分片，不使用 `fetch`、CDN 或网络。

## 5. Markdown 固定结构

1. Trace 身份与证据质量。
2. Query-native 扫描身份、覆盖率和候选入口。
3. 性能结论总览。
4. Frame Class 分布。
5. Bottleneck Signature 地图。
6. 关键路径与等待传播。
7. 独立 Evidence Cards。
8. 候选结论与证据索引；详细 Candidate Analysis Trail 留在机器 JSON，正文不重复逐项“如何发现”。
9. CPU 内存与 GPU 显存专项。
10. Tracy Runtime 采集与 Tracy Analysis 离线压力。
11. Evidence Gaps 与 Investigation Backlog。
12. 优化优先级和复测方案。
13. 全域健康统计附录。
14. 查询审计索引和九份专项报告链接。

无异常域只出现在健康附录，不为满足模板强造正文。

每张卡必须让读者看到：影响/频率/Debt、典型/最严重/重复实例、Primary Limiter、完整因果链、支持和反证、调用栈/Source/Pass/资源链、根因或最小缺口、优化方向、风险、理论上限、复测指标、置信度和状态。

## 6. 固定视觉证据

普通卡在对应证据存在时生成：

1. 帧上下文多轨 Timeline。
2. Signature 聚焦 Zone Timeline。
3. 因果关系图。
4. FrameImage（若存在）。
5. Stack/Source。

内存/显存卡：

1. 全 Trace 趋势与预算线。
2. 峰值/异常窗口。
3. 类型和增量。
4. Pass→Resource→Allocation/Heap。
5. 对应 FrameImage（若存在）。

原生 Profiler PNG 使用独立资源路径嵌入 Markdown，并关联同一定位记录的已验证交互入口；缺少能力或验收失败时明确展示缺口。静态 SVG 与 MCP PNG 仅作为来源明确的辅助图嵌入 Markdown。Timeline 没有包含可验证 Zone 坐标的 Evidence 时不生成 Timeline SVG；没有已证明 typed relation 时不生成因果 SVG；资源关系 Evidence 没有可验证节点时不生成资源图。Markdown 必须用文字披露缺失原因，禁止生成只有 `EvidenceUnavailable`/`EvidenceGap` 字样的空图。只有显式启用 HTML 时，才附加 `visuals/index.html#signature-<id>` 或 `#frame-<id>` 链接。

静态布局必须满足：

- 同一 Lane 的嵌套或时间重叠 Zone 分配到互不覆盖的行，Lane 高度按实际行数动态计算。
- Zone、Lane 和标题文字不得越过其可用区域；显示文本可省略，但 `<title>` 保留完整值。
- 因果节点使用动态高度和多行文本；关系箭头不得穿过节点文字。
- 单时间点内存证据渲染为带名称和值的快照比较图，不伪装成趋势折线。
- 无数据不生成 SVG 文件；旧输出目录中的同名陈旧 SVG 必须在重建时清理。

Profiler Canvas 原生截图单独按 2.3 合同登记，不是采集 FrameImage，不套用下面的 MCP 来源规则。采集 FrameImage 必须来自 MCP `resources/read` 返回的 `image/png`，保存 resource URI、MIME 和 PNG SHA-256。报告生成器拒绝 Skill 内本地解码或重建的图片。

## 7. Timeline LOD

```text
Complete
VisualAggregationOnly
QueryPartial
QueryTruncated
EvidenceUnavailable
```

`EvidenceUnavailable` 是报告质量状态，不是要求生成空 SVG。小于像素的 Zone 初始可显示密度带，但放大后必须恢复真实 Zone；LOD 只影响显示，不影响统计或结论。Query partial/truncated 必须显示质量警告。

## 8. 单 Trace 开销措辞

主报告固定包含：

```text
TotalCaptureOverhead = NotMeasuredSingleTrace
```

可以报告 Trace 内观测到的 producer 自耗时、Sampling、Context Switch、FrameImage、GPU Reference、Catalog、写盘和积压，但不能写成 Tracy 总体开销。

## 9. 确定性与安全

- `--deterministic` 使用 Analysis Result 内固定生成时间和稳定排序。
- 同一输入、配置、生成器和模板版本的确定性分析核心产物 SHA-256 一致。原生 PNG 与运行时验证记录单独保存实测身份，不承诺未经验证的跨环境像素确定性。
- 所有 Trace 字符串做 Markdown 转义；启用 HTML 时同时做 HTML/JavaScript 转义。
- 文件名只使用生成的稳定 ID，不使用 Marker/Resource 名称。
- 可选静态报告 HTML 不读取 `.tracy`，不要求 Query 常驻；独立 Profiler 运行时需要实际加载 Trace 和可用服务。
- `manifest.json` 记录版本、配置 SHA 和所有产物身份。

## 10. 完成门禁

- 所有显著 Signature 有 Card，达到 L6 或有最小 EvidenceGap。
- Wait 归因有真实 relation 或 `UnresolvedWaitChain`。
- GPU 推断与内存口径符合证据边界。
- 默认产物不包含 HTML，所有 Markdown 链接可解析。
- 静态图不存在文字/Zone/节点重叠；无坐标或无关系的卡片不产生占位 SVG。
- 显式启用 HTML 时，Markdown/HTML 数字、结论和 Evidence ID 一致，静态报告锚点可解析且可离线双击打开；可交互 Profiler 不作双击 HTML 即可运行的承诺。
- 两次确定性构建的分析核心产物 SHA 一致；原生视图按独立展示合同验证。
- 所有需要证据调查的分流项须记录实际回执、最深可信层与缺口；新 P1 筛选排除须有原始事实与理由，不能仅凭 Backlog 一句话跳过。覆盖计数与 Candidate Manifest 一致；P0 兼容保留但当前不生成，Query 原级别不改写，分析级别按 2.2 映射。原 P2（新 P1）入队但未完成证据报告会阻止完成；已经完成证据报告但未被选择源码深查，不阻止本轮规定范围完成。阶段结果明确标注未完成，保存调查队列，不使用完整报告的 passed/completed 状态。
- `analysis.capture_quality` 必须原样保留 Candidate Manifest/Query summary 中的 `capture_quality`，位于主报告附录和质量附件，不得作为 Candidate Finding、性能 P0/P1/P2、强制 L6 调查或性能 Backlog。附注只写问题、受影响域和限制；性能候选确实依赖缺失数据时才关联 EvidenceGap 深查。
- 旧 quality Candidate 拒绝进入新报告，必须用 candidate-policy-v3 重新生成；扫描器自身 checksum/identity/audit 错误仍阻止发布。
- 每个重大结论均可追溯到 Candidate ID、Query 数值路径、代表帧、Evidence ID 和 Source revision（若使用 Source）。
- Query 数值与 Candidate Manifest 逐字段一致；AI 不得重算或改写。
- `report-validation.json.passed=true` 表示产物合同校验成功，不独立表示分析完成。完整 Query-native 报告同时要求 `report_status=complete`、`analysis_complete=true`、`required_pending=0`；阶段报告使用 `in_progress/false` 并保留实际待查数。`build_report.py --validate-only` 和 `validate_analysis_result.py` 返回同一组状态字段。
- 经完整无过滤查询证实没有 CPU Zone 的 FrameSet 代表区间，只能按 candidate-investigation 中的 `verified_absent` 合同写 Unresolved，且在 `evidence_gaps` 显式保留候选关联和最小补证要求；不能写成健康、0 CPU 耗时或跳过其他代表帧。
- 具体 CPU 事件的 `verified_not_recorded` 和 Native Wait 的 `completion_not_recorded` 仅按 candidate-investigation 的完整原始收据合同结束为 Unresolved。主报告和专项报告必须可见 candidate_id、结束 outcome、Frame/原始 Evidence 引用、blocked_by 和最小补证；不能把“调查完成”呈现成“根因闭环”。原 partial、历史失败或开放时间边界保留，已补齐者通过独立新回执说明解决过程。

2.1正文以原因条目为单位，每条只完整解释一次。九份专项报告按域引用正文；未入选原始候选仍完整保存于机器清单。普通build_report.py自动执行新合同；历史回归必须显式使用--legacy-regression，且analysis_complete永远为false。新报告写入空目录以保留历史版本。
