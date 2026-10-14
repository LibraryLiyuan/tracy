> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 实际操作手册：从任务选择到产物验证

> 首版：2026-09-07｜命令与结论均需按适用版本阅读。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 先选择工作流

| 输入/目标 | 先读什么 | 本次应得到什么 |
|---|---|---|
| 已有 .tracy | [配置兼容](config-compatibility.md)、AI 链路 | 带身份的查询会话、调查与报告 |
| 已有 .tracy-stream | [采集/转换合同](../02-chains/01-capture.md)、存储链路 | 正常结束证明、转换结果和校验 |
| 新录制 | 采集工具目录和场景 ready 合同 | 独立任务目录、stream、日志和结束状态 |
| 构建/替换 Player | staging 合同与三仓版本地图 | Plan、独立副本、manifest、Validate、运行验收 |
| 调试 Query/Session | 对应分支和失败报告 | 有界复现、输入身份、差分证据 |
| 文档交接 | [协作规范](../07-collaboration/maintenance.md) | 专题状态、证据、下一步 |

当前文档任务没有执行这些运行命令。下面命令展示已核对的参数形式；将路径换成本次确认的实际值后使用。

## 1. 只读环境盘点

```powershell
$repoPath = 'C:/CodeProjects/GodotProjects/tracy-0.13.1'
git -C $repoPath branch --show-current
git -C $repoPath rev-parse HEAD
git -C $repoPath status --short --branch
git -C $repoPath worktree list

$toolPath = 'C:/本次确认的工具目录/tracy-query.exe'
Get-Item -LiteralPath $toolPath
Get-FileHash -LiteralPath $toolPath -Algorithm SHA256
```

若沙箱身份与仓库所有者不同且确认是目标仓库，可对单条只读命令使用 git -c safe.directory=<该仓库绝对路径>，不改全局配置。本次 Git 快照采用此方式。它不替代写操作权限。

## 2. 使用旧 JSON 采集辅助脚本

以下为同一任务的顺序示例。ResolvedProfile 必须是旧解析脚本生成的 JSON，不能直接使用 YAML。

```powershell
$skillPath = 'C:/Users/Admin/Documents/JN-Unity-T3/skill-draft/diagnose-jn-unity-tracy'
$taskPath = 'C:/Users/Admin/Documents/JN-Unity-T3/PlayerCaptures/本次独立任务'
$resolvedPath = 'C:/本次确认目录/resolved-profile.json'

& "$skillPath/scripts/start-capture.ps1" -ResolvedProfile $resolvedPath -TaskDirectory $taskPath -TaskId 'capture-unique-id'
& "$skillPath/scripts/monitor-capture.ps1" -ResolvedProfile $resolvedPath -TaskDirectory $taskPath
& "$skillPath/scripts/stop-capture.ps1" -TaskDirectory $taskPath -StopSource StopFile -WaitSeconds 180
```

监控单次调用不能描述为已经常驻监控；需要按任务节奏重复并保存记录。ApplyProtection 会执行保护动作，使用前确认本次保护策略。ManualCtrlC 模式只等待/验证人工停止，不代发按键。[S30](../08-references/source-index.md#s30) [S31](../08-references/source-index.md#s31) [S32](../08-references/source-index.md#s32)

## 3. 转换与校验分开

convert-stream.ps1 的 Mode 为 Convert 或 Finalize；参数包括 ResolvedProfile、TaskDirectory、StreamPath、OutputTrace，Finalize 还涉及 ValidationEvidence。Convert 先生成 .converting 候选产物，不能仅凭转换进程成功就称为已发布有效 Trace。读完整脚本和对应合同后提交验证证据。[S33](../08-references/source-index.md#s33)

传统 .tracy 转换、N29 sidecar 与 N30 Session 不共用一个通用“全部完成”判据；Session 发布要满足自己的 Canonical/Derived/Audit 合同。[S19](../08-references/source-index.md#s19) [S21](../08-references/source-index.md#s21)

## 4. 新 QueryScan 分析

1. 使用实际确认的 Query 1.35 EXE，记录 hash、MCP 初始化和 tools/list。
2. 按新合同传入 analysis-root，以及位于其中的 analysis-cache-root；现有旧 start-query-mcp.ps1 不完整提供这些参数，见兼容说明。
3. 通过 MCP describe 和 scan profile_validate 验证能力、Schema 与 Profile。
4. 打开唯一输入并等 ready；记录质量。
5. scan start → status；完成后取得全部候选页。
6. 保存原始回执并逐项调查；定期保存 in_progress。
7. 通过报告和 investigation audit 后才结束为 complete。
8. 需要让出资源时 cancel，并等待 CancelledResumable 回执。

操作依据 [S12](../08-references/source-index.md#s12) [S13](../08-references/source-index.md#s13)。不能在正式分析中用一次性 --request CLI 绕开常驻 MCP；开发验收器的独立测试路径另按其合同解释。

## 5. 构建与 Player

旧指南提供 CMake/Jam 目标示例；在对应分支核对目标和构建目录后执行。Player 更新使用现有 staging 工具完整 Plan/Deploy/Validate，不把复制 DLL 当完整交付。长期构建、资源副本、目标启动和配置变更应按本次任务已有授权及 staging 合同执行。[S01](../08-references/source-index.md#s01) [S29](../08-references/source-index.md#s29)

## 每次运行收尾

写明实际退出码、时间、输入/产物 hash、配置、成功与失败范围、仍存活的本任务进程及恢复入口。日志有 PASS 不代表所有阶段 PASS；保留失败运行，不用后来成功覆盖原始报告。
