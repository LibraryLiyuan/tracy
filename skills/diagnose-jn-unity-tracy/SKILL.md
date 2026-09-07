---
name: diagnose-jn-unity-tracy
description: Use when diagnosing a single existing JN Unity Tracy capture with Query-native deterministic scanning and an auditable performance report.
metadata:
  author: JN Game
  version: 2.0.1
---

# JN Unity × Tracy 单 Trace 深度诊断

使用 Query 1.35 对一个已存在的 `.tracy` 做确定性全量扫描，再由 AI 深查候选、核对源码并生成可审计报告。本 dev 基线发行版仅支持普通 `.tracy`，不包含 N30 Session Store。Query 拥有全部统计数值；AI 只负责证据驱动的解释、假设验证、源码核查和优化建议。

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

涉及 GPU 资源时再读取 [gpu-resource-catalog.md](references/gpu-resource-catalog.md)。参考文件不再递归路由。

## 唯一配置权威

`config/JNTracy.AnalysisProfile.yaml` 是预算、Marker 规则、候选策略、用户关注和资源限制的唯一运行时权威。使用安全 YAML loader 读取，然后通过同一个常驻 MCP 的 `tracy_scan(operation="profile_validate")` 交给 Query 严格校验和规范化。

把以下内容落盘：

- 原始 YAML SHA-256。
- Query 返回的 normalized Profile。
- `profile_identity`。
- Profile validation MCP 请求与响应。

Skill 中的 JSON 旧配置只属于 1.1.2 回归资产，不得参与 2.0 分析。Python 只可安全解析 YAML、维护状态和渲染报告，不得读取 Trace 或重新统计性能数据。

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
8. 将 `summary/quality.capture_quality` 原样保存为报告附录；不进入候选或调查队列。所有 `selected=true`、P1、`user_focus` 候选都是必查项；优先级只决定顺序。未调查项保留在 Backlog，不算完成。
9. 对每个队列候选调用 `candidate_get` 和 `representative_frames`，按候选专项做定向 MCP 查询、源码核查、支持/反证记录。
10. 所有必查候选完成后生成 Analysis Result、Evidence Manifest 和 Analysis State；Backlog 不得隐去。
11. 先校验 Query 数值未被改写，再运行确定性报告生成器和完整报告校验。
12. 报告通过后调用 `tracy_scan(close)` 与 `tracy_trace_close`，保存最终状态。

每一步的退出条件和失败语义见 [query-native-scan-workflow.md](references/query-native-scan-workflow.md)。

## 调查覆盖规则

Query Candidate Manifest 是候选和数值事实的权威：

- P1 与 `user_focus`：强制完成调查。P0 仅保留为协议/枚举兼容值，当前策略不生成 P0；P1/P2/P3/P4 不自动前移。
- `capture_quality`：只作为附录简述问题、受影响域和分析限制，不分配性能优先级，不占候选/调查名额，不默认深查。仅当具体性能候选依赖缺失数据时，在该候选下关联 EvidenceGap 并定向补查。
- 旧 Manifest 含 `domain=quality` 或 `trigger=quality` 时不得沿用；使用 `candidate-policy-v3` 重新生成，不能把质量项降成 P1/P2 或静默改写旧证据。
- 扫描器自身 checksum、identity、计数审计失败仍然阻止发布/使用候选；与源 Capture 已声明的域缺失严格区分。
- 所有 `selected=true`：强制调查，包括 P2/P3/P4；不能因低优先级、已有若干结论或 Backlog 已写原因而跳过。
- 其余：保留在 Investigation Backlog，并披露未调查贡献和原因。
- 已完成 candidate ID 不得在恢复时重复调查。
- Query 提供的 percentile、inclusive/exclusive、Debt、次数、比例、峰值和容量值必须逐字段引用，AI 不得重算或改写。
- partial、truncated、分页未完成、domain invalid 或第三次查询失败必须成为 EvidenceGap，不能解释为空。

候选并不自动等于根因。最长 Zone、Top Marker、Sampling 热点、显存峰值只建立假设；根因必须沿真实线程、Job、Fence、Submission、GPU Pass、Allocation 或 I/O relation 验证。

## MCP 与源码账本

每个逻辑请求最多三次：原范围、缩小/分页、拆分实体。全部请求记录 method、canonical params、attempt、时间、响应路径/SHA、cursor、partial/truncated、Trace/scan identity 和质量。

每条源码证据记录仓库、revision、文件、行、符号、内容 SHA 和与 Trace revision 的匹配状态。revision 不匹配时只能形成 Hypothesis，不能形成 Confirmed 源码结论。

所有显著结论都要同时能回到：

```text
Candidate trigger
→ Query numeric facts
→ representative frame / event refs
→ directed MCP evidence
→ typed relation or explicit EvidenceGap
→ source evidence（如适用）
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

## 报告完成门禁

AI 只提交结构化 Analysis Result、Evidence Manifest、Analysis State 和文字分析字段。Query 数值必须原样引用 Candidate/Evidence。运行：

```powershell
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

完成前还必须运行 Skill Python/PowerShell 测试和 Skill linter。报告要包含完整可审计分析过程、所有 selected/P1/UserFocus 的调查结果、Backlog、Evidence Gaps、质量附录、优化方向、功能风险、理论收益边界和可执行复测指标。任何 selected 尚未调查时，只交付明确标注未完成的阶段结果，保存 `investigating_candidates` 状态；不得标为 completed/completed_with_degradation。已调查但证据受限必须有实际查询及缺口记录，不能把未查询改称 EvidenceGap。默认不修改源码、不发送外部消息。
