> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 仓库、分支与成套版本地图

> 首版：2026-09-07｜命令与结论均需按适用版本阅读。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 现场已核对的入口

| 工作对象 | 实际位置/分支 | 用途与边界 |
|---|---|---|
| 引擎主工作区 | C:/workflow/jnunity；当前为 feature/JNTracy-DXR-GpuTiming-20260907 | DXR 专项，不能默认认为检出 feature/JNTracy |
| Package 主工作区 | C:/workflow/PackageRepo；feature/Pack-JNTracy | Managed 桥及编译开关 |
| Tracy 主工作区 | C:/CodeProjects/GodotProjects/tracy-0.13.1；dev | 团队集成基线 |
| QueryScan | tracy-0.13.1-ai-query-scan | 1.35 开发；有未提交 tracked 修改 |
| N29 | tracy-0.13.1-n29-gpu-analysis | GPU sidecar |
| N30/N30A | tracy-0.13.1-n30-long-trace-session | Session Store 与全域审计 |
| GUI | tracy-0.13.1-profiler-gui | N27 GUI preview |
| StreamConvert | tracy-stream-convert-opt | 离线转换性能与语义验证 |
| Skill 草案 | 本工作区 skill-draft/diagnose-jn-unity-tracy | metadata 2.0.1 |
| Skill 安装版 | C:/Users/Admin/.codex/skills/diagnose-jn-unity-tracy | metadata 1.1.3 |

Tracy worktree 简写均位于 C:/CodeProjects/GodotProjects。详细 commit、跟踪关系、工作区和文件差异见 [jnunity](jnunity.md)、[PackageRepo](PackageRepo.md)、[Tracy](tracy.md)。

## 三个不同的版本事实

1. **Git HEAD**：源码提交身份。
2. **工作区修改**：未提交但可能参与构建的实际内容。
3. **二进制 SHA 与运行合同**：本次实际执行的文件和支持能力。

QueryScan HEAD 为 QSCAN13 提交，但现场有 24 项 tracked 修改；用 HEAD 标签判断它等同历史 QSCAN13 会遗漏 QSCAN14 改动。反过来，磁盘有修改不证明旧 EXE 已重建。

[repository-snapshot.json](../evidence/repository-snapshot.json) 保存完整本地 refs、merge-base、left/right、recent commits、changed files 和 worktree tracked 状态。本次没有 fetch、没有枚举 untracked、没有重建 EXE。个别旧引擎分支无法取得本地 merge-base，标为不可比较。

## 配套路线

| 要做的事 | 应先核对的组合 | 不应混用 |
|---|---|---|
| 复现旧 N27 采集 | 旧报告三仓 commit、Protocol90/section12/Query1.32 | 任意当前 N28/QueryScan 工具 |
| N28 来源图开发 | 三仓 N28 专项实现及 Schema3 | 只换单个 DLL/Query |
| N29 sidecar | N29 Converter/Builder/Query/Profiler 合同 | dev 上未具备能力的 EXE |
| N30A Session | N30 worktree、Query1.34 历史合同、有效 Session | building 目录、未验收路径 |
| 新 AI 确定性扫描 | QueryScan 1.35 + 草案2.0.1 + YAML Profile | 安装1.1.3的旧 JSON 流程 |
| DXR timing-only | DXR 引擎代码及实际 DLL/PDB、配套 Player | 由源码 HEAD 猜 DLL 身份 |

这张表是选择资料的入口，不是新配套组合的兼容认证。历史版本来源 [S01](../08-references/source-index.md#s01) [S17](../08-references/source-index.md#s17) [S19](../08-references/source-index.md#s19) [S22](../08-references/source-index.md#s22) [S27](../08-references/source-index.md#s27) [S28](../08-references/source-index.md#s28)。

## 分支文档维护

新增分支时补目标、创建基线、实际差异、配套分支、验证范围、worktree 和合并意图；合并后记录实际 commit/PR，不能凭相似代码或 ahead/behind 推断。归档分支保留历史用途，避免删除后失去报告对应版本。
