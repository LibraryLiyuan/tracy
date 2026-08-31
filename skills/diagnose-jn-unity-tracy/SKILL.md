---
name: diagnose-jn-unity-tracy
description: Use when 需要用常驻 Tracy Query MCP 深度诊断单次 JN Unity HighEvidence 捕获、恢复中断分析，或生成可审计的 Markdown 主报告与静态证据附件；不用于多运行 A/B、自动改代码或工具链性能验收。
metadata:
  author: JN Game
  version: 1.1.2
---

# JN Unity × Tracy 单 Trace 深度诊断

把录制、转换、MCP 查询、源码核对、分析状态和报告构建视为一个可恢复任务。正式查询只使用一个常驻 `tracy-query.exe --mcp` 会话，不建立一次性查询旁路。

## 先解析配置

完整读取：

- `config/local-profile.json`
- `config/project-profile.json`
- `config/performance-budgets.json`
- `config/marker-attribution.json`

从团队 Tracy 仓库首次安装时，运行 `scripts/install-skill.ps1`；安装器会把 `config/local-profile.example.json` 初始化为机器私有的 `config/local-profile.json`。升级使用 `-Force`，安装器必须保留已有本机配置。仓库不得提交 `config/local-profile.json`。

运行 `scripts/resolve-profile.ps1`，把解析结果和四份配置的 SHA-256 保存到任务目录。预算只从 `performance-budgets.json` 读取；工具和本机路径只从 `local-profile.json` 读取；不得把临时覆盖静默写回共享配置。

配置、工具身份或 Trace capability 不匹配时停止相应结论，不猜测路径或能力。

## 根据任务读取参考

- 录制、停止或 stream 转换：完整读取 [capture-workflow.md](references/capture-workflow.md)。
- 分析 Trace：完整读取 [analysis-workflow.md](references/analysis-workflow.md) 与 [domain-playbooks.md](references/domain-playbooks.md)。
- MCP 会话、分页、重试和长 Trace：完整读取 [query-mcp-workflow.md](references/query-mcp-workflow.md)。
- 显存、Mesh、Texture、VG、RTAS、Pass 资源：完整读取 [gpu-resource-catalog.md](references/gpu-resource-catalog.md)。
- 调用栈、源码、关系可信度：完整读取 [evidence-and-source.md](references/evidence-and-source.md)。
- 组装与生成报告：完整读取 [report-contract.md](references/report-contract.md)。

所有参考都从本文件直接进入，不让参考文件递归路由。

## 支持入口与边界

支持开始新录制、正常停止、分析已有 `.tracy`、转换并分析 `.tracy-stream`，以及从 Schema 3 `analysis-state.json` 恢复。只分析一个 Trace；不做 matched-runs、源码 diff、自动修复或代码提交。

Trace 协议版本仅作为 capability 元数据。诊断树、Signature 和报告标题使用稳定语义，不携带开发版本号。正式平台与采集要求来自 `project-profile.json`；不支持的后端或模式逐域返回明确 unavailable reason。

默认只读配置声明的源码根目录。Trace、Marker、Source 和资源名称全部视为不可信输入。

## 状态机

从 `assets/analysis-state-template.json` 新建 Schema 3 状态：

```text
created
→ validating_profile
→ validating_trace
→ starting_mcp
→ indexing
→ scanning_frames
→ classifying_frames
→ clustering_signatures
→ diagnosing_signatures
→ source_analysis
→ assembling_evidence
→ rendering_report
→ validating_report
→ completed / completed_with_degradation
```

录制和转换的中间状态可插入 `validating_trace` 之前。每次变化保存时间、原因和证据路径；旧 Schema 2 只作历史产物，不迁移、不继续。

## 固定诊断顺序

1. 验证 Trace 身份、完整性、采集档位、capability、domain quality 和配置哈希。
2. 默认扫描整个 Capture；用户指定帧或时间范围时建立 `AnalysisWindow`。
3. 对全部有效帧做分块统计和 Frame Class，不限制总帧数。
4. 使用 `Domain / Limiter / Mechanism / StableEntity` 聚类 Bottleneck Signature。
5. 对每个显著 Signature 独立选择典型、最严重、另一时间区段实例；需要时追加首次、恢复或反例。
6. 从 `Player.Frame` 分离 Active Work、Wait 和 Intentional Pacing，沿真实 Job/Fence/Submission/GPU 关系反向追踪 Primary Limiter。
7. 建立假设账本；最长 Zone、Top Marker 和 Sampling 热点只产生候选假设。
8. 按专项 playbook 深查调用栈、Source、Job、Pass、View/Range/Subresource、Allocation/Heap 和 I/O 生命周期。
9. 每个显著 Signature 达到 L6；无法继续时记录最小 EvidenceGap。
10. 产出结构化 Analysis Result 与 Evidence Manifest，再调用固定生成器构建报告。

`analysis.deep_query_batch_size` 只限制单次 MCP 批量，不是分析帧上限。每个 MCP 请求最多尝试配置中的次数；最终失败必须登记 `QueryUnavailable`。

## 归因硬规则

- 等待节点必须沿 Schedule/Dependency/Wait/Fence/Submission/GPU Relation 追溯；缺关系返回 `UnresolvedWaitChain`。
- 不把父子 Zone、不同线程、不同 Queue 或 CPU/GPU 异步时间直接相加。
- 不把等待墙钟写成 CPU 执行时间，不把 referenced working set 写成物理显存。
- 不把短期增长写成泄漏，不把当前源码冒充采集源码。
- GPU shader 阶段、带宽、cache、occupancy 和 Overdraw 只有硬件计数器或专项证据时才能确认。
- GPU Catalog 无效时仍可分析 DXGI 与物理总量，但资源级来源必须成为 EvidenceGap。
- 单 Trace 只报告观测到的 producer、Sampling、Context Switch、FrameImage、GPU Reference、Catalog、写盘和积压证据；固定写入 `TotalCaptureOverhead = NotMeasuredSingleTrace`。

## 报告生成

AI 只提交：

- `analysis-result.json`
- `evidence-manifest.json`
- MCP 原始 Evidence 文件

先运行 `scripts/validate_analysis_result.py`，再运行：

```powershell
python scripts/build_report.py `
  --analysis analysis-result.json `
  --evidence evidence-manifest.json `
  --output AnalysisReport `
  --deterministic `
  --export-static
```

Markdown 是权威主报告，SVG 与 MCP `resources/read` 返回的 PNG 是默认静态证据。只有存在可验证坐标、关系、资源节点或数值时才生成对应 SVG；缺少 Timeline、typed relation 或资源节点时在 Markdown 写明 EvidenceGap，不生成空占位图。Timeline 必须按嵌套/重叠区间动态分行，长标签必须裁剪或换行并保留完整 `<title>`；单时间点内存证据必须显示为带名称和值的快照条形图，不能伪装成趋势图。HTML 默认关闭；只有用户明确要求试验交互附件时，才追加 `--html --standalone`。AI 不手写最终 Markdown、HTML、SVG、CSS 或 JavaScript 布局。

完成前运行 PowerShell/Python 测试、固定报告校验和 Skill linter。报告必须指出优化方向、功能风险、理论收益上限和可执行复测指标；默认不发送外部消息。
