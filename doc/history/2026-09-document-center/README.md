> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# JN Unity × Tracy AI 性能分析开发文档中心

> 首版生成：2026-09-07。路径：C:/Users/Admin/Documents/JN-Unity-T3/JNTracy-Docs。
> 本中心将现有系统、链路、迭代与操作资料串联起来；历史验收不被改写，未完成事项保留真实边界。

## 新对话从这里开始

2026-10-10 新增：[Tracy AI 性能分析工作梳理与交接讨论稿](../2026-10-10-inventory/work-inventory.zh-CN.md) · [对话与资料索引](../2026-10-10-inventory/sources.zh-CN.md)。涉及当前交接时先读这份日期明确的底稿；下列首版页面中的版本与状态仍需按其 2026-09-07 快照理解。

1. 读 [当前状态与下一步](STATUS.md)，识别正在开发、历史通过与当前阻塞。
2. 初次了解系统，读 [系统架构](01-system/architecture.md) → [能力矩阵](01-system/capabilities.md) → [数据与版本契约](01-system/contracts.md)。
3. 继续开发，读对应 [关键链路](02-chains/README.md)、[迭代记录](03-iterations/README.md)、[分支说明](04-branches/README.md)。
4. 执行工具前读 [操作手册](05-operations/runbook.md) 和 [配置兼容](05-operations/config-compatibility.md)。
5. 收尾按 [维护规则](07-collaboration/maintenance.md) 保存证据与交接；[启动提示词](07-collaboration/starting-prompts.md) 可直接复制到新任务。

## 文档导航

| 目录/文档 | 内容 |
|---|---|
| [01 系统架构](01-system/architecture.md) | 职责、数据流、支持能力、对象与版本合同 |
| [02 七条关键链路](02-chains/README.md) | 采集、转换查询、CPU/Job、GPU/DXR、资源显存、AI、构建交付 |
| [03 开发迭代](03-iterations/README.md) | N00–N25基础，N26/N27/N28/N31资源，N29/N30A存储，QSCAN与GPU专题 |
| [04 分支与工作区](04-branches/README.md) | 三仓分支目标、HEAD、差异、配套、验收边界及原始快照 |
| [05 操作手册](05-operations/runbook.md) | 使用顺序、版本核对、采集/转换/查询/部署 |
| [工具目录](05-operations/tool-catalog.md) | EXE与脚本用途、已核对参数、产物与限制 |
| [配置兼容](05-operations/config-compatibility.md) | 安装1.1.3与草案2.0.1，JSON/YAML及启动器差异 |
| [故障排查](05-operations/troubleshooting.md) | 已知症状、证据、下一步与错误处理方式 |
| [06 指标口径](06-quality/metrics.md) | 帧、CPU/GPU、Job、内存、预算和因果解释 |
| [验收标准](06-quality/validation.md) | 层级、证据包、完成状态与差分判据 |
| [架构决策](06-quality/decisions.md) | 11项关键决策、原因和适用状态 |
| [07 协作维护](07-collaboration/maintenance.md) | 多对话更新、记录模板和恢复提示词 |
| [08 来源索引](08-references/source-index.md) | 38份原始资料、路径与文件hash |
| [待核实事项](08-references/known-gaps.md) | 开发阻塞、证据缺口和后续文档建设 |
| [本次建设记录](sessions/2026-09-07-documentation-bootstrap.md) | 生成范围、发现与验证 |
| [文档检查结果](evidence/document-validation.json) | 文件清单、链接/结构检查与来源漂移 |

## 文档事实规则

- 文档记录的状态必须附范围与来源；Planned、Implemented、单元通过、真实验收通过、发布是不同状态。
- Git快照只代表采样时本地refs；未fetch、未枚举untracked，也没有验证所有二进制。
- 来源索引中的hash证明文件身份，不代表复验原报告。
- QueryScan/Session等设计目标不能写成所有当前分支已实现的能力。
- 新增资料优先链接原报告，避免复制后出现多个互相矛盾的“最新版本”。

## 最常用入口

了解系统：[架构](01-system/architecture.md)。找问题链路：[链路地图](02-chains/README.md)。选择代码分支：[三仓地图](04-branches/README.md)。操作工具：[工具目录](05-operations/tool-catalog.md)。跨对话接续：[启动提示词](07-collaboration/starting-prompts.md)。

本次仅生成Markdown文档及JSON证据资料；Word/PDF手册可在内容审核或对外分享时另行导出。

