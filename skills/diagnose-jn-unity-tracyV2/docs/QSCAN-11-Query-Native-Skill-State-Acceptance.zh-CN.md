# QSCAN-11 Query 原生 Skill 与状态机阶段验收

日期：2026-09-06  
状态：Passed（发布前包装警告见第 5 节）  
Skill 草案：`diagnose-jn-unity-tracy` 2.0.0  
Query 契约：1.35.0  
Analysis State：Schema 4

## 1. 本阶段结果

- `SKILL.md` 已从 1.1.2 的四份 JSON、Python 分块扫描流程，改为 Query 1.35 原生确定性扫描编排。
- `JNTracy.AnalysisProfile.yaml` 成为预算、Marker、候选策略、UserFocus 和资源限制的唯一运行时权威。
- 正式 Profile 解析不再启动一次性 Query CLI；规范化必须来自任务中唯一常驻 MCP 的已保存响应。
- Analysis State 升级为 Schema 4，持久化 MCP、Trace、Scan、Candidate、Investigation、MCP/Source/Hypothesis Ledger 和报告状态。
- `CancelledResumable` 使用原 scan ID 恢复；Complete 的 Aggregate/Candidate identity 可复用；重复响应不会重复追加状态历史。
- Candidate 分页只有在最后一页 `done=true`、`next_cursor=null`、总数一致且 ID 无重复时才可进入调查。
- P0、P1 和 `user_focus` 全部强制入队；其他 `selected=true` 候选进入普通队列；全部未调查候选保留在 Backlog。
- 已完成 candidate ID 在恢复后不会再次入队。
- 新增 Query 数值镜像校验；AI 改写 `trigger_evidence` 中任意数值、scope、unit 或 authority 都会失败。
- Capture/stop/convert 仍为单独路由，不混入 Query 扫描状态。

## 2. 新增或重写文件

```text
SKILL.md
assets/analysis-state-template.json
scripts/manage-analysis-state.py
scripts/resolve-analysis-profile.py
references/query-native-scan-workflow.md
references/candidate-investigation.md
references/capture-workflow.md
references/domain-playbooks.md
agents/openai.yaml
tests/test_skill_v2_contract.py
tests/test_profile_resolution.py
tests/test-scripts.ps1
```

## 3. 测试

| 门禁 | 结果 |
|---|---|
| QSCAN-11 新测试先行 | 预期失败：缺状态脚本、Skill 1.1.2、Schema 3、旧 JSON 路由 |
| Python unittest | 27/27 Passed |
| PowerShell 综合测试 | Passed |
| Distribution 回归 | Passed |
| Profile 缺失/危险 YAML | Passed，立即阻断 |
| Query schema 1.34 | Passed，立即阻断 |
| CancelledResumable | Passed，返回 resume |
| Aggregate 重复应用 | Passed，无重复状态历史 |
| P0/P1/UserFocus 覆盖 | Passed |
| 已完成候选去重 | Passed |
| AI 数值篡改 | Passed，校验失败 |
| Skill Creator spec validator | Passed |
| Agent Skill Linter 可执行规则 | 0 Error；3 个发布包装 Warning；5 个目录 Info |

## 4. QSCAN-11 阶段结束快照 SHA-256

下列值用于复现 QSCAN-11 阶段结束状态；QSCAN-12 会继续更新报告命令与 `SKILL.md`，因此不应把这些值当作草案目录的当前实时 SHA。

```text
SKILL.md
E3501F8EE2539F8B4B828F43E4E69DD1F95D01D203B747171F782CBDCA0B3C96

assets/analysis-state-template.json
66690B801AAA2F9D883CB2125E4CBDEBAE3D0EAFA09355261DB4BB7674F66249

scripts/manage-analysis-state.py
3A74C284FB987872012B541173E5AF46C6E09B80B3930DC203181F3B8C1E3707

scripts/resolve-analysis-profile.py
91D89A7C3FE7D823CD57D3DFF3CC2079DA3D335B60D97A5B405E45E968DB50D9

references/query-native-scan-workflow.md
A6FA7A4E5CA2AE49AE1748B5D4DF7DD04354A9E4F6A9B95E1352BF1BED9C7F04

references/candidate-investigation.md
B039E6C937E9D5702F3212BAD61AF29C4878AF2DC45107762D7E4D801B154AF7

config/JNTracy.AnalysisProfile.yaml
895E2FD4B4543971AD45D903E34FAF07E7C2D504D6789AF6938C523D1F084F6D
```

## 5. Linter 说明

本机 `agent-skill-linter` CLI 缺少其自身未随包提供的 `click`、`rich` 和 `skills_ref` 运行依赖，不能直接启动。已分别执行：

1. `skill-creator/scripts/quick_validate.py`：`Skill is valid!`。
2. `agent-skill-linter` 除外部 `skills_ref` 规则之外的全部规则：0 Error。

剩余 Warning 是草案目录尚无 `LICENSE`、`README.md` 和 GitHub CI。没有擅自为用户代码指定开源许可证；这些属于 QSCAN-14 安装/发布包装决策，不影响 QSCAN-11 运行时正确性。非标准 `agents/config/docs/schemas/vendor` 目录属于本 Skill 的有意结构。

## 6. 未进入本阶段

- 报告 Schema 的 Candidate/Analysis Trail 强约束在 QSCAN-12 完成。
- 9 份专项报告和 Manual Verification Guide 在 QSCAN-12 完成。
- 两份真实 `.tracy` 与 N30 Session 集中验收在 QSCAN-13 完成。
- 已安装的 `C:\Users\Admin\.codex\skills\diagnose-jn-unity-tracy` 未修改；必须等待 QSCAN-14 用户批准。
