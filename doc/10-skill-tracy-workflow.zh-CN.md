# 普通 Trace 的 Query MCP Skill 工作流

普通 .tracy 沿用 Query schema 1.35.0、candidate-policy-v3、Schema4 状态和既有报告合同。V2 中仍保留这部分；Stream 新路线有独立入口，不能以新包版本直接改写旧运行的 schema 或完成含义。

## 输入与配置

一个完成转换并可加载的 .tracy、合法 JNTracy.AnalysisProfile.yaml、匹配 Query MCP、授权源码根和独立输出目录。local-profile.json 提供机器路径，YAML 提供预算和候选策略；两者职责不同。

首先让 Query profile_validate 规范化 YAML，保存原文件 SHA、normalized Profile、profile_identity 和请求响应。不能由 Python 或模型重新解释一套与 Query 不同的阈值。[query-native-scan-workflow.md:1](../skills/diagnose-jn-unity-tracyV2/references/query-native-scan-workflow.md#L1)

## 状态与恢复

```text
created → profile_validated → mcp_ready → trace_ready
→ scan_running → scan_complete / scan_cancelled_resumable / scan_failed
→ candidate_manifest_pending → investigating_candidates
→ assembling_report → validating_report
→ completed / completed_with_degradation
```

CancelledResumable 使用原 scan_id 恢复；已完成 Scan 复用相同 Aggregate/Candidate identity；Profile 变化按该实现的中立统计复用规则处理。身份或 checksum 错误不能以空结果继续。旧 Schema2/3 保持历史状态，不静默迁移成 Schema4。

## 标准执行顺序

1. 记录 EXE SHA、常驻 MCP 进程身份、initialize、tools/list 与 describe。
2. 打开 Trace 并等待 ready，保存输入身份、完整性、capability 和域质量。
3. 验证 Profile，调用 Scan start，轮询 status；不按固定等待时间推断完成。
4. 完成后读取 summary/quality，完整分页 candidates 和必要 signatures。
5. 生成多维候选视图和分流记录，保留 Query 原优先级、数值和触发原因。
6. 对强制或已批准项做定向证据与源码深查，保存真实实例、父链、关系和反例。
7. 生成新 P1 决策报告、分析正文、backlog 与质量附录，执行数值/结构校验。
8. 独立登记原生展示状态，最后 close Scan 与 Trace。

当前 tracy_scan 有旧 12 个操作加 triage：profile_validate、start、status、cancel、resume、summary、signatures、candidates、candidate_get、representative_frames、quality、close、triage。能力检查应判断所需集合和实际版本，不能沿用旧固定数量断言。[TracyMcpServer.cpp:1](../query/src/TracyMcpServer.cpp#L1) [TracyAnalysisScanManager.cpp:1](../query/src/TracyAnalysisScanManager.cpp#L1)

## 优先级和调查范围

| 原 Query 编号 | 分析编号与流程 |
|---|---|
| P1 | 新 P0，强制源码深查 |
| P2 | 新 P1，逐项筛选并生成证据决策报告，由测试人员选择源码深查 |
| P3 / P4 | 新 P2 / P3，保留原 selected 等语义 |
| 用户关注 | 按合同提升到新 P0 |
| Capture quality | 单独附录，不伪装成性能 P0 |

不能重复递推编号，不能把“未选择 P1 深查”写成源码受阻或已深查。全部 selected 都应有分流记录；文章篇幅、Top N 或工具调用成本不能充当丢弃理由。

## 证据和源码账本

每个逻辑请求记录参数、attempt、返回路径/SHA、游标、范围、质量和 Trace/scan identity。partial、truncated、漏页、失败和缺失能力保留为 EvidenceGap。调查停止条件只关闭相应证据查询，不自动完成源码深查义务。

源码记录至少有仓库/revision、文件与行、符号、实际快照、机制、触发条件、已有优化，以及与事件的对应。源码版本未匹配时保持 Hypothesis；源码存在某种风险，不等于运行中的这一次一定走了该分支。

## 结果校验与生成

以下脚本基于已保存的 Query 结果工作，不解析完整 Trace：

```powershell
python scripts/prepare-review-routing.py --candidates candidate-manifest.json --output review-routing-draft.json
python scripts/classify-candidates.py --candidates candidate-manifest.json --output candidate-views.json
python scripts/manage-analysis-state.py validate-numerics --analysis analysis-result.json --candidate-manifest candidate-manifest.json
python scripts/validate_analysis_result.py --analysis analysis-result.json --evidence evidence-manifest.json --candidates candidate-manifest.json
python scripts/build_report.py --analysis analysis-result.json --evidence evidence-manifest.json --candidates candidate-manifest.json --output AnalysisReport --deterministic --export-static
```

命令在所选 Skill 根目录执行。legacy-regression 只做历史回归，不能绕过当前深查规则形成正式完成声明。

完成状态同时报告 analysis_complete、required_pending、tester_decision_pending、source_scope_complete 和 all_selected_source_complete。完整报告只证明本轮规定范围；根因与收益的确认程度单独呈现。旧生成器通过也不能代替原生图片和真实交互验收。
