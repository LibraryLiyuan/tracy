# JN Unity Tracy AI 性能分析交接文档

这套文档面向接手开发维护的同事，也提供从采集到分析报告的操作流程。当前说明以 **2026 年 10 月 14 日本地 Tracy mainline、Unity feature/JNTracyUnity、PackageRepo feature/Pack-JNTracy** 为基线，历史聊天整理和验收原件保存在 history 中。

## 先读哪些内容

| 读者任务 | 阅读顺序 |
|---|---|
| 接手整个项目 | [当前状态](00-current-state.zh-CN.md) → [系统架构](01-system-architecture.zh-CN.md) → [源码地图](02-tracy-source-map.zh-CN.md) → [交接与维护](17-maintenance-and-handoff.zh-CN.md) |
| 采集并生成报告 | [采集与自动登录](05-capture-and-autologin.zh-CN.md) → [Skill 版本与安装](08-skill-versions-and-installation.zh-CN.md) → [Stream 分析](09-skill-stream-workflow.zh-CN.md) → [报告与回查](12-profiler-views-and-share.zh-CN.md) |
| 修改 Unity 或 Package | [引擎改动](03-unity-engine-changes.zh-CN.md) → [Package 改动](04-packagerepo-changes.zh-CN.md) → [构建部署](13-build-deploy-and-reproduce.zh-CN.md) → [验证](14-validation-and-real-cases.zh-CN.md) |
| 修改 Query 或长录制 | [存储与格式](06-storage-formats-and-long-capture.zh-CN.md) → [Query/MCP/analyze](07-query-mcp-and-analyze.zh-CN.md) → [问题与待办](16-troubleshooting-and-open-items.zh-CN.md) |
| 了解历史设计和案例 | [演进与分支](15-history-decisions-and-branches.zh-CN.md) → [历史归档入口](history/README.zh-CN.md) → [聊天索引](appendices/conversations.zh-CN.md) |

## 当前先要明确的四件事

1. **mainline 不等于 dev。** 本地 dev 含 mainline 之外的 4 个提交，涉及 Query 归并修复与分析器内存上限。本文不会把 dev 独有修复写成 mainline 已有行为。
2. **Skill 有三个位置。** 仓库旧目录是 2.3.1，仓库 V2 和本机安装版是 2.6.3；V2 与安装版的分析器 EXE 不同。包版本、源码版本和二进制身份要分别记录。
3. **Stream 当前主流程是 Journal 加 EventStore。** Capture GUI 边录制边建库，停止后发布 stream 与 store；Stream Skill 调用 tracy-analyze，不必转换完整 .tracy。普通 .tracy 仍有独立的旧 Query/MCP 工作流。
4. **历史通过只对相应输入与工具成立。** 10 月 13 日有完整 Stream 分析实例；它使用安装的 32 GiB 修订分析器，不是本轮 mainline 源码重新构建的验收。

## 文档目录

| 文档 | 内容 |
|---|---|
| [00 当前状态](00-current-state.zh-CN.md) | 三仓、Skill、二进制、分支关系、实现与验证状态 |
| [01 系统架构](01-system-architecture.zh-CN.md) | 职责、整体数据流、关键链路和设计边界 |
| [02 Tracy 源码地图](02-tracy-source-map.zh-CN.md) | 模块入口、调用路径、改动影响范围 |
| [03 Unity 引擎改动](03-unity-engine-changes.zh-CN.md) | 单 DLL、CPU/Job/GPU/资源/I/O、最新 Writer 与修复 |
| [04 PackageRepo 改动](04-packagerepo-changes.zh-CN.md) | C#/Lua、GPU Pass、RenderGraph、资源加载、自动化 |
| [05 采集与自动登录](05-capture-and-autologin.zh-CN.md) | 配置、启动、GUI/CLI、停止、产物检查 |
| [06 存储格式与长录制](06-storage-formats-and-long-capture.zh-CN.md) | stream、store、skeleton、tracy、jnidx、N30、固定切片 |
| [07 Query MCP 和 analyze](07-query-mcp-and-analyze.zh-CN.md) | 入口选择、请求、数据语义、预算、错误和恢复 |
| [08 Skill 版本与安装](08-skill-versions-and-installation.zh-CN.md) | 目录对应、差异、安装器、工具配置与身份 |
| [09 Stream Skill 流程](09-skill-stream-workflow.zh-CN.md) | 全量筛选、审计、严重帧取证、源码调查、报告 |
| [10 普通 Trace Skill 流程](10-skill-tracy-workflow.zh-CN.md) | Query 1.35、YAML、状态机、候选分流和旧合同 |
| [11 分析质量与因果](11-report-quality-and-causality.zh-CN.md) | 指标、实例、对照、源码、根因和复测要求 |
| [12 Profiler 回查与分享](12-profiler-views-and-share.zh-CN.md) | 新原生网页通道、旧 Wasm 路线、截图、响应性和便携交付 |
| [13 构建部署与复现](13-build-deploy-and-reproduce.zh-CN.md) | CMake、Bee/Jam、IL2CPP、Client 补丁与完整 Player |
| [14 验证与真实案例](14-validation-and-real-cases.zh-CN.md) | 验收层级、已保存实例、执行时间和资源口径 |
| [15 历史决策与分支](15-history-decisions-and-branches.zh-CN.md) | N 系列、QSCAN、Skill、存储路线和纠正记录 |
| [16 故障排查与待办](16-troubleshooting-and-open-items.zh-CN.md) | 症状、检查入口、已知差异和未完成事项 |
| [17 文档维护与交接](17-maintenance-and-handoff.zh-CN.md) | 接手步骤、材料清单、更新责任与复核方式 |

附录：[引擎完整文件清单](appendices/engine-file-changes.zh-CN.md)、[Package 完整文件清单](appendices/packagerepo-file-changes.zh-CN.md)、[Tracy 变更范围](appendices/tracy-file-changes.zh-CN.md)、[配置矩阵](appendices/configuration-matrix.zh-CN.md)、[Skill 文件说明](appendices/skill-files.zh-CN.md)、[源码入口](appendices/source-index.zh-CN.md)、[文档漂移](appendices/known-document-drift.zh-CN.md)、[聊天与资料](appendices/conversations.zh-CN.md)。

## 如何理解文档中的状态

字段与语义速查见 [数据契约](appendices/data-contracts.zh-CN.md)。

“源码已实现”“已编译”“某样本通过”“完整业务运行通过”和“优化收益已测量”分别记录。当前说明来自本地源码、分支差异、配置与已有验收记录；本轮只做文档和身份核对，没有重新跑完整产品或性能测试。

文件差异以指定比较基线的 merge-base 到目标 HEAD 计算，包含第三方、测试和元数据，不能把总行数直接当作独立功能代码量。[采样证据](evidence/repository-snapshot.json) 与 [来源登记](evidence/source-registry.json) 保存完整身份。

同仓源码使用相对链接；Unity、Package 和工作区证据使用本机绝对路径。换电脑时按当前状态页的仓库映射定位；原始 Trace、大型报告包和构建产物没有复制进 Git 文档目录。
