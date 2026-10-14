# Skill 文件与职责

V2 为 167 个文件，安装版为 173 个，排除 pycache 等生成目录。完整文件/大小/SHA 在 [清单](../evidence/skill-inventory.json)。

## 脚本

| 脚本 | 职责 |
|---|---|
| [audit-marker-attribution.ps1](../../skills/diagnose-jn-unity-tracyV2/scripts/audit-marker-attribution.ps1) | 历史 Marker 归属审计支持 |
| [build_report.py](../../skills/diagnose-jn-unity-tracyV2/scripts/build_report.py) | 旧结构化结果到报告入口 |
| [candidate_manifest.py](../../skills/diagnose-jn-unity-tracyV2/scripts/candidate_manifest.py) | 候选清单合同 |
| [classify-candidates.py](../../skills/diagnose-jn-unity-tracyV2/scripts/classify-candidates.py) | 保留原数值的候选多维视图 |
| [confirmation_evidence.py](../../skills/diagnose-jn-unity-tracyV2/scripts/confirmation_evidence.py) | 确认状态证据 |
| [convert-stream.ps1](../../skills/diagnose-jn-unity-tracyV2/scripts/convert-stream.ps1) | 旧转换路径，新原生分析不必经过它 |
| [install-skill.ps1](../../skills/diagnose-jn-unity-tracyV2/scripts/install-skill.ps1) | 事务式安装、备份、机器配置保留 |
| [investigation_receipts.py](../../skills/diagnose-jn-unity-tracyV2/scripts/investigation_receipts.py) | 调查回执身份与完整性 |
| [lint-skill.py](../../skills/diagnose-jn-unity-tracyV2/scripts/lint-skill.py) | Skill 静态结构与引用检查 |
| [manage-analysis-state.py](../../skills/diagnose-jn-unity-tracyV2/scripts/manage-analysis-state.py) | 旧状态、数值和调查审计 |
| [monitor-capture.ps1](../../skills/diagnose-jn-unity-tracyV2/scripts/monitor-capture.ps1) | 采集状态监控 |
| [performance_report.py](../../skills/diagnose-jn-unity-tracyV2/scripts/performance_report.py) | 旧详细正文生成 |
| [performance_review.py](../../skills/diagnose-jn-unity-tracyV2/scripts/performance_review.py) | 深查字段合同 |
| [prepare-review-routing.py](../../skills/diagnose-jn-unity-tracyV2/scripts/prepare-review-routing.py) | 候选分流草稿 |
| [query_native_report.py](../../skills/diagnose-jn-unity-tracyV2/scripts/query_native_report.py) | 旧 Query 报告支持 |
| [report_common.py](../../skills/diagnose-jn-unity-tracyV2/scripts/report_common.py) | 报告公共支持 |
| [resolve-analysis-profile.py](../../skills/diagnose-jn-unity-tracyV2/scripts/resolve-analysis-profile.py) | 分析 Profile 解析 |
| [resolve-profile.ps1](../../skills/diagnose-jn-unity-tracyV2/scripts/resolve-profile.ps1) | 本机路径配置解析 |
| [review_routing.py](../../skills/diagnose-jn-unity-tracyV2/scripts/review_routing.py) | 分流和待选择/批准状态 |
| [select-analysis-frames.ps1](../../skills/diagnose-jn-unity-tracyV2/scripts/select-analysis-frames.ps1) | 历史选帧辅助，不能替代全量候选覆盖 |
| [start-capture.ps1](../../skills/diagnose-jn-unity-tracyV2/scripts/start-capture.ps1) | 历史采集 wrapper，使用前核对工具版本 |
| [start-query-mcp.ps1](../../skills/diagnose-jn-unity-tracyV2/scripts/start-query-mcp.ps1) | 普通 Trace 的常驻 Query MCP 启动 |
| [stop-capture.ps1](../../skills/diagnose-jn-unity-tracyV2/scripts/stop-capture.ps1) | 正常停止封装 |
| [stream_review.py](../../skills/diagnose-jn-unity-tracyV2/scripts/stream_review.py) | 指定 manifest 的 CPU 窗口调查义务，不合并其他补证目录 |
| [stream_views.py](../../skills/diagnose-jn-unity-tracyV2/scripts/stream_views.py) | 读取机器设置并转发 prepare/start/export |
| [stream_workflow.py](../../skills/diagnose-jn-unity-tracyV2/scripts/stream_workflow.py) | Stream run、audit、追加窗口计划；不解析原始 Trace |
| [validate_analysis_result.py](../../skills/diagnose-jn-unity-tracyV2/scripts/validate_analysis_result.py) | 旧分析结果合同校验 |

## 参考资料

| 参考 | 所属范围 |
|---|---|
| [analysis-workflow.md](../../skills/diagnose-jn-unity-tracyV2/references/analysis-workflow.md) | 旧合同或跨路线证据/领域规则 |
| [candidate-investigation.md](../../skills/diagnose-jn-unity-tracyV2/references/candidate-investigation.md) | 旧合同或跨路线证据/领域规则 |
| [capture-workflow.md](../../skills/diagnose-jn-unity-tracyV2/references/capture-workflow.md) | 旧合同或跨路线证据/领域规则 |
| [cause-investigation.md](../../skills/diagnose-jn-unity-tracyV2/references/cause-investigation.md) | Stream 与原因调查 |
| [deep-analysis-and-report.md](../../skills/diagnose-jn-unity-tracyV2/references/deep-analysis-and-report.md) | 旧合同或跨路线证据/领域规则 |
| [domain-playbooks.md](../../skills/diagnose-jn-unity-tracyV2/references/domain-playbooks.md) | 旧合同或跨路线证据/领域规则 |
| [evidence-and-source.md](../../skills/diagnose-jn-unity-tracyV2/references/evidence-and-source.md) | 旧合同或跨路线证据/领域规则 |
| [gpu-resource-catalog.md](../../skills/diagnose-jn-unity-tracyV2/references/gpu-resource-catalog.md) | 旧合同或跨路线证据/领域规则 |
| [long-capture-triage.md](../../skills/diagnose-jn-unity-tracyV2/references/long-capture-triage.md) | 旧合同或跨路线证据/领域规则 |
| [native-profiler-views.md](../../skills/diagnose-jn-unity-tracyV2/references/native-profiler-views.md) | 旧合同或跨路线证据/领域规则 |
| [priority-and-tester-review.md](../../skills/diagnose-jn-unity-tracyV2/references/priority-and-tester-review.md) | 旧合同或跨路线证据/领域规则 |
| [query-mcp-workflow.md](../../skills/diagnose-jn-unity-tracyV2/references/query-mcp-workflow.md) | 旧合同或跨路线证据/领域规则 |
| [query-native-scan-workflow.md](../../skills/diagnose-jn-unity-tracyV2/references/query-native-scan-workflow.md) | 旧合同或跨路线证据/领域规则 |
| [report-contract.md](../../skills/diagnose-jn-unity-tracyV2/references/report-contract.md) | 旧合同或跨路线证据/领域规则 |
| [stream-analysis.md](../../skills/diagnose-jn-unity-tracyV2/references/stream-analysis.md) | Stream 与原因调查 |
| [stream-native-views.md](../../skills/diagnose-jn-unity-tracyV2/references/stream-native-views.md) | Stream 与原因调查 |

## 二进制 配置和模板

- bin 中是具体程序，不能由包版本推定构建来源。
- tools/stream-report-viewer 包含服务、页面、导图和验证工具。
- 机器配置与示例分开；安装器保留配置时仍可能改变 EXE。
- schemas/assets/templates 保留旧合同，不能随包版本自动改写历史结果。

[版本与安装](../08-skill-versions-and-installation.zh-CN.md) 说明实际差异和试装步骤。
