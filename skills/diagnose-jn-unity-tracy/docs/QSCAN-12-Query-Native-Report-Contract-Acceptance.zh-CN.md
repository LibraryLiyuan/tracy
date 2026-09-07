# QSCAN-12 Query 原生报告契约阶段验收

日期：2026-09-06  
状态：Passed  
Skill 草案：`diagnose-jn-unity-tracy` 2.0.0  
Query 契约：1.35.0  
Analysis Result：Schema 2（增加 Workflow 2.0 条件字段）

## 1. 本阶段结果

- 报告生成器接收 Query 1.35 Candidate Manifest，并校验文件 SHA-256、Aggregate identity 和 Candidate content identity。
- `analysis-result.json` 增加 `query_scan`、`candidate_findings` 与 `investigation_backlog`。
- 每个 Candidate Finding 必须逐字段复制 Query `trigger_evidence` 和 `representative_frame_details`；AI 改写数值或代表帧即失败。
- 每个 Candidate 必须记录完整 Analysis Trail：发现、统计、代表帧选择、逐层归因、假设、源码核查、反证和结论。
- `Confirmed` 必须由 typed relation、revision 一致的 Source，或至少两份独立 Evidence 支持；不得把 Marker/时间接近度伪装成确定因果。
- 所有 P0、P1、UserFocus Candidate 必须完成调查；其余未完成项必须逐项进入 Backlog。
- 每个正文 Bottleneck Signature 必须存在 Candidate 触发路径。
- 单 Trace 仍固定使用 `TotalCaptureOverhead = NotMeasuredSingleTrace`，不生成虚假的 A/B 总开销结论。
- 主报告保留完整分析过程，并确定性生成九份专项报告：Trace 质量、CPU、GPU、Job、Memory、I/O/System、Tracy 开销、Profiler 人工回查、Backlog/Evidence Gap。
- Profiler 人工回查指南直接列出 Query 选出的 Frame ID、原因、事件引用和应展开的线程/GPU Direct 轨道。
- Legacy Workflow 1.x 报告仍可生成；Workflow 2.0 才强制 Candidate 契约。

## 2. 新增或修改文件

```text
scripts/query_native_report.py
scripts/build_report.py
scripts/validate_analysis_result.py
scripts/report_common.py
schemas/analysis-result.schema.json
references/report-contract.md
SKILL.md
tests/test_query_native_report_contract.py
```

## 3. 固定输出

```text
Performance-Analysis-Report.md
01-Trace-Quality-and-Scan-Coverage.md
02-Frame-and-CPU-Analysis.md
03-GPU-and-Render-Analysis.md
04-Job-and-Scheduling-Analysis.md
05-Memory-and-GPU-Resource-Analysis.md
06-IO-Sampling-and-System-Analysis.md
07-Tracy-Telemetry-Overhead.md
08-Profiler-Manual-Verification-Guide.md
09-Investigation-Backlog-and-Evidence-Gaps.md
analysis-result.json
candidate-manifest.json
evidence-manifest.json
manifest.json
report-validation.json
```

## 4. 测试

| 门禁 | 结果 |
|---|---|
| QSCAN-12 测试先行 | Passed；实现前因缺少 Candidate 报告契约而失败 |
| Query-native 专项 unittest | 6/6 Passed |
| 全部 Python unittest | 33/33 Passed |
| PowerShell 综合脚本测试 | Passed |
| Distribution 回归 | Passed |
| 缺 Candidate trigger path | 正确拒绝 |
| 缺代表帧选择理由 | 正确拒绝 |
| AI 改写 Query 数值 | 正确拒绝 |
| 无可审计依据的 Confirmed | 正确拒绝 |
| Source revision 不匹配的 Confirmed | 正确拒绝 |
| 两次 deterministic 构建 | 主报告及九份专项报告 SHA-256 一致 |
| Analysis Result Schema JSON | 解析通过 |

## 5. 关键 SHA-256

```text
67C22FC588C9D375DA988C8470BC9D94F37FF933981BC0E056774E36043ADC8E  SKILL.md
E88DC5C1DE24738EDAF3435300C65D407917098A6655BF65C9F0E4F9733CE33F  scripts/build_report.py
E3045DE97EB2C017A5A04EED09757DE4ED9E4D68D65A5FEE66CF2D16F232D3A6  scripts/validate_analysis_result.py
C6ED38F8CAF246C8E64F17B7F2A51BBCE320CA89E0C75C18696398590DEE1C25  scripts/query_native_report.py
CFE0BA4BBFA36AEFA02462D7973C3BD66494BFB7DE283FFD2749CB3B26ACD5D0  scripts/report_common.py
295F5F769E800A9FEFFC7EFE6280EC10FA40CDE9449236DE8ABCFBABCB9F686E  schemas/analysis-result.schema.json
15A342965F337779DC7AA401CDF10C9BADC690738B57FF075A647ABDA73EC74F  references/report-contract.md
```

## 6. 发布边界

- 已安装的 `C:\Users\Admin\.codex\skills\diagnose-jn-unity-tracy` 未修改。
- QSCAN-11 文档中的 SHA-256 是 QSCAN-11 阶段结束快照；`SKILL.md` 随 QSCAN-12 报告命令更新后 SHA 已变化。
- 本阶段只验证 synthetic/fixture 契约；真实 GPU Catalog Off Trace、G05 Trace 与 N30 Session 属于 QSCAN-13。
- Agent Skill Linter 本机安装缺少其自身 `click`、`rich`、`skills_ref` 依赖；QSCAN-11 已对当前前端元数据执行等价可运行规则且为 0 Error。QSCAN-12 没有改变 frontmatter。
