> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 工具目录与便利操作入口

> 首版：2026-09-07｜命令与结论均需按适用版本阅读。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 正式工具链

| 工具 | 用途 | 输入 → 输出 | 使用限制 |
|---|---|---|---|
| tracy-capture.exe | 连接 Client 并实时采集 | 地址/端口/配置 → .tracy-stream、日志 | 只写 stream，按正常 drain 合同停止 |
| tracy-stream-inspect.exe | Journal 结构/完整性检查 | stream → 检查结果 | 不是性能分析报告 |
| tracy-stream-convert.exe | 离线回放/转换 | stream → .tracy；N29/N30 产物依分支 | 需精确工具版本与发布验证 |
| tracy-query.exe | 离线事实查询，QueryScan 分支另支持原生扫描 | Trace/Session + MCP → 事实/候选 | 正式 AI 分析保持一个 MCP；核对 Schema |
| tracy-profiler.exe | 人工查看和复核 | 支持格式 → Timeline/GPU/Memory 视图 | GUI Preview/Session 支持依分支 |
| tracy-gpu-analysis-build.exe | N29 sidecar 派生构建/恢复 | raw sidecar → Derived | generation、lease、checksum 有独立合同 |

工具角色见 [S01](../08-references/source-index.md#s01) [S19](../08-references/source-index.md#s19) [S21](../08-references/source-index.md#s21)。本次没有逐个运行 EXE --help，具体可选参数以选定版本为准。

## 已核对接口的脚本

脚本根目录：[开发草案 scripts](C:/Users/Admin/Documents/JN-Unity-T3/skill-draft/diagnose-jn-unity-tracy/scripts)。

| 脚本 | 参数入口 | 主要行为/产物 | 状态 |
|---|---|---|---|
| resolve-profile.ps1 | ProjectProfile、Budgets、MarkerAttribution、LocalProfile、OutputPath | 解析四份 JSON 配置 | 旧合同；已读入口 |
| start-capture.ps1 | ResolvedProfile、TaskDirectory、TaskId；Address/Port 可覆盖 | stream/stop/log 路径、capture-process.json | 已读启动正文；拒绝覆盖 |
| monitor-capture.ps1 | ResolvedProfile、TaskDirectory、ApplyProtection | 检查本任务 PID+启动时间、文件和资源 | 已读入口；保护动作按配置 |
| stop-capture.ps1 | TaskDirectory、StopSource、WaitSeconds | StopFile 或等待 ManualCtrlC | 已读入口；不是任意终止进程工具 |
| convert-stream.ps1 | Mode、ResolvedProfile、TaskDirectory、StreamPath、OutputTrace、ValidationEvidence | Convert/Finalize、候选转换和验证 | 已读入口；发布前读完整合同 |
| start-query-mcp.ps1 | ResolvedProfile、TraceRoot、SessionManifestPath | --mcp --allow-root 及 source roots | 旧入口；未传新版 analysis-root/cache-root |

来源 [S30](../08-references/source-index.md#s30) [S31](../08-references/source-index.md#s31) [S32](../08-references/source-index.md#s32) [S33](../08-references/source-index.md#s33) [S34](../08-references/source-index.md#s34) [S35](../08-references/source-index.md#s35)。静态阅读不构成本次运行测试。

## 新 AI 状态与报告辅助

| 文件 | 已有合同用途 | 注意 |
|---|---|---|
| manage-analysis-state.py | ingest-candidates、调查队列、record-investigation-audit | 处理落盘结果，不扫描 Trace |
| investigation_receipts.py | 请求/响应和实体范围审计相关实现 | 以报告/调查合同为准 |
| resolve-analysis-profile.py | 新 Analysis Profile 解析相关入口 | 与 resolve-profile.ps1 的四 JSON 合同不同 |
| build_report.py / query_native_report.py | 报告构建与 Query-owned 数值约束 | in_progress 和 complete 必须区分 |
| validate_analysis_result.py | 结构化结论/报告校验相关入口 | 不能以校验脚本通过代替真实调查 |
| install-skill.ps1 | 安装/更新 Skill | 本次未执行；草案未完成 FW6 安装 |

文件存在已核对，用途依 [S12](../08-references/source-index.md#s12) [S13](../08-references/source-index.md#s13) [S15](../08-references/source-index.md#s15)；本次未逐个验证 Python CLI 参数，不提供猜测的一键命令。

## Player 部署工具

[Stage-JNTracyPlayerCode.ps1](C:/Users/Admin/.codex/skills/jntracy-stage-player-code/scripts/Stage-JNTracyPlayerCode.ps1) 提供 Plan/Deploy/Validate。先读 [staging 合同](C:/Users/Admin/.codex/skills/jntracy-stage-player-code/SKILL.md) 与机器默认参数说明，再创建本次计划。它管理独立完整副本与替换集合，不是覆盖原 Player 的复制脚本。[S29](../08-references/source-index.md#s29)

## 资料整理和历史报告脚本

工作区 [tools](C:/Users/Admin/Documents/JN-Unity-T3/tools) 中存在 collect_tracy_workflow_evidence.py、build_tracy_workflow_execution_report.py、verify_tracy_workflow_execution_report.py、render_unity_tracy_real_trace_report.py、translate_tracy_manual.py 等历史脚本。它们应按原任务资料使用；本次只盘点文件，不认证其适配 Query 1.35，也不把旧提取/统计脚本列为新生产扫描路径。

新文档中心提供 [新对话提示词](../07-collaboration/starting-prompts.md)、[记录模板](../07-collaboration/templates.md)、[故障索引](troubleshooting.md)，作为无需运行脚本的便利入口。
