> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 配置、版本兼容与迁移注意事项

> 首版：2026-09-07｜命令与结论均需按适用版本阅读。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 两个并存入口

| 项目 | 已安装 1.1.3 | 开发草案 2.0.1 |
|---|---|---|
| Skill 路径 | C:/Users/Admin/.codex/skills/diagnose-jn-unity-tracy | 本工作区 skill-draft/diagnose-jn-unity-tracy |
| 主要配置 | local-profile.json、project-profile.json、performance-budgets.json、marker-attribution.json | config/JNTracy.AnalysisProfile.yaml |
| 解析入口 | resolve-profile.ps1 | 新 Profile 校验/Query profile_validate |
| Query 合同 | 以安装版及其配置实际要求为准 | schema 精确 1.35.0 |
| 主分析方式 | 旧常驻 MCP 深度诊断流程 | Query 全量扫描 → 候选 → AI 调查 |
| 当前发布状态 | 现场文件存在，metadata 已读 | 草案，QSCAN14 FW6 尚未完成 |

[S27](../08-references/source-index.md#s27) [S28](../08-references/source-index.md#s28) [S35](../08-references/source-index.md#s35)。版本字符串只标识文件声明，不能替代其所有脚本、依赖和 EXE hash。

## 已发现的文档/脚本接口差异

新 query-native-scan-workflow 要求 --analysis-root 和其内部 --analysis-cache-root；现存 start-query-mcp.ps1 只组装 --mcp、--allow-root 和 --allow-source-root。该旧 wrapper 不能直接被宣传为完整的新流程入口。[S12](../08-references/source-index.md#s12) [S34](../08-references/source-index.md#s34)

采集辅助脚本仍接收 ResolvedProfile JSON；新 SKILL 入口以已有 Trace + YAML 为主，将新录制/转换路由到辅助合同。因此“新 Profile 只有一份”不能被理解为已经删除所有采集端旧 JSON 工具。

早期设计举例包含工具/源码路径，新 capture-workflow 又要求采集机器路径和权限不进入共享预算配置；实际配置位置应先按正在使用的入口和 Schema 核对，不复制早期例子覆盖本机配置。[S07](../08-references/source-index.md#s07) [S14](../08-references/source-index.md#s14)

## 迁移应核对的顺序

1. 记录安装版、草案、Query EXE 和配置 hash。
2. 对照新 Schema 检查字段及默认值，不把 JSON 换扩展名当 YAML 迁移。
3. 确认启动参数能允许本次 analysis/cache roots，并能完成 MCP preflight。
4. 用指定短 Trace 完成真实扫描、全部强制调查、回执审计与报告。
5. 明确失败/未完成项；满足发布合同后再安装，保留机器私有配置和旧版恢复入口。

本次仅记录差异，没有修改 Skill 或执行升级。

## 缓存身份

缓存与 Trace、Query build、scan/aggregate/policy algorithm 和 Profile identity 绑定。变化后须校验是否可复用；不能通过手工改旧 manifest 的版本字段伪装兼容。改预算通常只需 Policy 重评估，但仍要证明确实复用了兼容的中立聚合。[S12](../08-references/source-index.md#s12)
