> 历史归档：2026 年 10 月 10 日交接整理。当前版本与操作入口见 [文档首页](../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# Tracy 交接对话与资料索引

日期：2026 年 10 月 10 日。配合 [工作梳理与交接讨论稿](work-inventory.zh-CN.md) 使用。

已定位并读取 113 条相关或相邻聊天的关键轮次，共 288 轮。长聊天按关键近期轮次获取，再用专题文档补足架构和演进背景；本索引不表示逐条审计了所有聊天历史。聊天中的完成状态只证明当时明确限定的结果，当前状态另见仓库、配置、工具与部署证据。

## 优先阅读的聊天

| 用途 | 原聊天 |
|---|---|
| 现有文档体系及最初缺口 | [规划 Tracy AI 性能分析文档](codex://threads/01a07bb2-2bac-7611-bb9d-51328b969dff) |
| 最新 dev 合并、stream 接入及查询限制 | [检查 Tracy 合并冲突](codex://threads/01a11fc4-f913-7cc2-a0a8-4c2092d7289a) |
| 新引擎配置、显存、使用率及记录路径变化 | [切换到 JNTracyUnity 分支](codex://threads/01a11f9d-389d-7e21-ae93-8b9275b568c2) |
| Query 长数据、R20 与合入 dev | [检查 Query 长录制与 GPUCatalog](codex://threads/01a09ef3-6776-7173-9bb7-d8ffd26472bc) |
| 大文件打开失败及固定切片开发 | [Diagnose Tracy file crash](codex://threads/01a0ed47-7317-7df1-b432-86e25bb26ed7) |
| 分析深度、指标语义和报告格式 | [优化 SKILL 执行流程](codex://threads/01a11a32-c791-7b91-8c2a-16b15eba525f) |
| Skill 2.3.1 分册及报告交付 | [SKILL新版本试验](codex://threads/01a096de-d4c8-7c00-abb3-f98faf739a45) |
| 原生网页 Profiler 与截图接口 | [优化SKILL](codex://threads/01a0b2b1-ae65-71e3-8c2c-359f1193893c) |
| 最新 Player 构建及兼容包例外 | [试验 SKILL 并生成替换包](codex://threads/01a120a1-ff7c-76c2-b8b0-d410a7b16dad) |
| Player 构建模式和技能分发 | [迭代 JN Tracy Player 技能](codex://threads/01a09f5f-eea8-7750-a3b5-94ecacf5da81) |
| Marker 与父子路径对账 | [Compare Job worker traces](codex://threads/01a0e741-1c34-7fc2-971b-3c697825e910) |
| GUI 选择与实际采集档位不一致 | [检查录制配置与 GPU 数据缺失](codex://threads/01a0e753-ce39-79f3-872d-934e2a3ae7da) |
| N30 历史通过与后继差分失败 | [Unity Tracy 显存迭代计划 (2)](codex://threads/01a060f4-9c22-7880-b173-1ea6458b2246)、[Unity Tracy Session验收](codex://threads/01a0719c-b969-70f0-8c27-525114eeec86) |
| 早期流式采集设计与实现 | [分析 Unreal Insights 流式写盘](codex://threads/019f87e8-8291-70b2-a618-782a59bc6878) |

## 文档和实物入口

| 资料 | 入口 | 使用说明 |
|---|---|---|
| 文档中心 | [README](../2026-09-document-center/README.md) | 原首版内容为 9 月 7 日，状态需结合本次梳理 |
| 系统与能力 | [架构](../2026-09-document-center/01-system/architecture.md)、[能力矩阵](../2026-09-document-center/01-system/capabilities.md)、[契约](../2026-09-document-center/01-system/contracts.md) | 架构框架可复用，版本表和能力状态需更新 |
| 七条关键链路 | [链路目录](../2026-09-document-center/02-chains/README.md) | 采集、存储、CPU/Job、GPU/DXR、资源、AI、构建 |
| 开发迭代 | [迭代目录](../2026-09-document-center/03-iterations/README.md) | N00–N31 与 QSCAN 历史范围 |
| 三仓旧分支说明 | [分支目录](../2026-09-document-center/04-branches/README.md) | 用于理解目的与历史；当前 HEAD 以本次快照为准 |
| 日常操作与工具 | [操作手册](../2026-09-document-center/05-operations/runbook.md)、[工具目录](../2026-09-document-center/05-operations/tool-catalog.md) | 正式交接前需同步新入口和配置 |
| 分析口径与验收 | [指标](../2026-09-document-center/06-quality/metrics.md)、[验收](../2026-09-document-center/06-quality/validation.md)、[决策](../2026-09-document-center/06-quality/decisions.md) | 保留事实、假设、范围和缺失值语义 |
| 自动登录原件 | [N22 纠正与 N22B 验收](C:/Users/Admin/Documents/JN-Unity-T3/deployment-records/20260816/Player-T3-Auto-Login-and-Capture-Acceptance-2026-08-16.md) | N20/N21 自动登录结论已作废 |
| Render Thread 修复 | [验收报告](C:/Users/Admin/Documents/JN-Unity-T3/Render-Thread-Zone-Nesting-and-Coverage-Fix-Acceptance-20260906.zh-CN.md) | CPU 层级修复不替代 GPU 帧归属验证 |
| GPU 帧归属 | [OriginFrameId 交接](C:/Users/Admin/Documents/JN-Unity-T3/GPU-L0-OriginFrameId-Integration-Handoff.zh-CN.md) | 读取时核对实际实现版本 |
| N31 开销方案 | [Managed Catalog 计划](C:/Users/Admin/Documents/JN-Unity-T3/N31-Managed-GPU-Catalog-Performance-and-Lifecycle-Plan.zh-CN.md) | 计划不能当最终通过记录 |
| N30 窗口整合 | [GPU 资源窗口交接](C:/Users/Admin/Documents/JN-Unity-T3/N30-GPU-Memory-Resources-Window-Integration-Handoff.zh-CN.md) | 保留 Session 与 legacy 边界 |
| Query 缓存 | [优化状态](C:/Users/Admin/Documents/JN-Unity-T3/Query-Cache-Storage-Optimization/Implementation-Status.zh-CN.md) | 历史实验与新访问层分别判断 |
| Query R20 | [验收报告](C:/Users/Admin/Documents/JN-Unity-T3/GpuCatalog-DiskIndex/Query-Final-Acceptance-20260929-r20/Report.zh-CN.md) | 复用 R14 已发布索引，非重新冷建 |
| 固定切片 | [原计划](C:/Users/Admin/Documents/JN-Unity-T3/Tracy-Fixed-Slice-Plan.zh-CN.md)、[MCP 等价方案](C:/Users/Admin/Documents/JN-Unity-T3/Tracy-Fixed-Slice-MCP-Parity-Plan-20261007.zh-CN.md)、[实施证据](C:/Users/Admin/Documents/JN-Unity-T3/Tracy-Fixed-Slice-Evidence/20261007-mcp-parity-001/README.md) | 进行中，最终验收未完成 |
| 分析质量复盘 | [深度评审](C:/Users/Admin/Documents/JN-Unity-T3/Skill-Quality-Review-20261008/Tracy分析与报告质量深度评审.md)、[插桩与格式建议](C:/Users/Admin/Documents/JN-Unity-T3/Skill-Quality-Review-20261008/插桩分析与报告格式约束建议.md) | 有实际反例，最新建议未直接变成 Skill 实现 |
| Job 层级对账 | [SRP Batcher 精确核查](C:/Users/Admin/Documents/JN-Unity-T3/UnityProfiler-Tracy-JobWorker-20260928/SRPBatcher-Hierarchy-Audit.zh-CN.md) | 纠正同名事件和不同路径的误读 |
| QS17 阅读与交付 | [报告总览](C:/Users/Admin/Documents/JN-Unity-T3/QS17-Skill-2.3.1-ReportIteration/Typora/Performance-Analysis-Report.md)、[便携包说明](C:/Users/Admin/Documents/JN-Unity-T3/QS17-Skill-2.3.1-SharePackage/README.zh-CN.md) | 保留原报告的降级与展示限制 |
| 当前工具包 | [八个工具](C:/Users/Admin/Documents/JN-Unity-T3/Tracy-Integrated-3947331da)、[构建清单](C:/Users/Admin/Documents/JN-Unity-T3/Tracy-Integrated-3947331da/executables.json)、[验证记录](C:/Users/Admin/Documents/JN-Unity-T3/Tracy-Integrated-3947331da/validation.json) | 3947331da，Windows x64 Release，无 LTO 验证构建 |
| 最新 Player manifest | 部署清单（历史引用当前未找到：`C:/workflow/T3_Builds/t3_publication_cn_development_v0.0.2375.7327240_default/T3_development_JNTracy_20261009-203345/_JNTracyDeploymentManifest.json`） | 静态完成，runtime pending |
| 测试机部署 | [9 月 14 日说明](C:/Users/Admin/Documents/JN-Unity-T3/TestMachine-Handoff/2026-09-14/README.zh-CN.md) | 路径与分支基线需要更新，历史记录不表示本次已部署测试机 |
| 中文使用手册 | [Tracy 中文手册入口](C:/Users/Admin/Documents/JN-Unity-T3/output/tracy-manual-zh-CN/README.zh-CN.md) | 属上游操作学习材料，不能代替 JN 扩展说明 |
| 文件迁移记录 | [2026 年 9 月 11 日移动映射](C:/Users/Admin/Documents/JN-Unity-T3/History/Organization-20260911/move-map.csv) | 旧聊天绝对路径失效时先查询映射 |

## 本次核对证据

- [聊天查阅记录](evidence/thread-read-audit.json)：保留聊天身份、标题、读取轮次和覆盖范围。
- [统一聊天索引](evidence/thread-inventory.json)：相关关键词、项目入口与跨聊天引用的并集。
- [仓库快照](evidence/repository-snapshot.json)：本地分支、HEAD、tracked 修改和 dev 祖先关系。
- [工具哈希复核](evidence/tool-hash-check.json)：八个 EXE 均与已有清单一致。
- [配置对照](evidence/config-comparison.json)：默认 INI、独立 HighEvidence 及说明文档的版本和差异。
- [来源文件清单](evidence/source-file-manifest.json)：本次引用文件的存在性和 SHA-256。
- [文档链接检查](evidence/document-check.json)：新增文档本地目标检查结果。

“有文件”和“哈希一致”证明资料或工具的身份；功能与性能结论仍需回到具体运行和验收记录。

## 完整聊天导航

标题保留读取时的原名。创建日期采用 UTC+8，只用于查找聊天，不代表某项功能的实现或验收日期。“全部可读轮次”指接口本次未返回更早页面，不包含重新审计所有工具输出的含义。

| 聊天 | 创建日期 | 已读取轮数 | 查阅范围 |
|---|---|---:|---|
| [试验 SKILL 并生成替换包](codex://threads/01a120a1-ff7c-76c2-b8b0-d410a7b16dad) | 2026-10-09 | 3 | 全部可读轮次 |
| [检查 Tracy 合并冲突](codex://threads/01a11fc4-f913-7cc2-a0a8-4c2092d7289a) | 2026-10-09 | 8 | 部分关键轮次 |
| [切换到 JNTracyUnity 分支](codex://threads/01a11f9d-389d-7e21-ae93-8b9275b568c2) | 2026-10-09 | 2 | 部分关键轮次 |
| [查找超过10分钟的Tracy录制](codex://threads/01a11aa4-42e1-7d30-bbf5-980a5b90ec04) | 2026-10-08 | 1 | 全部可读轮次 |
| [优化 SKILL 执行流程](codex://threads/01a11a32-c791-7b91-8c2a-16b15eba525f) | 2026-10-08 | 6 | 部分关键轮次 |
| [重新编译 Tracy 可执行文件](codex://threads/01a0f138-c8ca-7de1-a44c-63b2c0480a4d) | 2026-09-30 | 2 | 部分关键轮次 |
| [分享 JN Tracy Skill 路径](codex://threads/01a0ed69-3595-7030-9ce5-e27a2f73d9f0) | 2026-09-29 | 1 | 全部可读轮次 |
| [Diagnose Tracy file crash](codex://threads/01a0ed47-7317-7df1-b432-86e25bb26ed7) | 2026-09-29 | 3 | 部分关键轮次 |
| [优化 Tracy 小问题](codex://threads/01a0ebfa-c439-7f93-b24b-d0dca127f609) | 2026-09-29 | 2 | 部分关键轮次 |
| [Query 长录制优化时间](codex://threads/01a0eb50-f8d2-7353-9d56-94627bb44ef2) | 2026-09-29 | 2 | 部分关键轮次 |
| [Compare Job worker traces](codex://threads/01a0e741-1c34-7fc2-971b-3c697825e910) | 2026-09-29 | 3 | 部分关键轮次 |
| [Identify Tracy GPU memory curve](codex://threads/01a0e761-ddf7-7842-a893-8f16159831b8) | 2026-09-28 | 1 | 全部可读轮次 |
| [检查录制配置与 GPU 数据缺失](codex://threads/01a0e753-ce39-79f3-872d-934e2a3ae7da) | 2026-09-28 | 3 | 部分关键轮次 |
| [查找补全 Tracy 调用栈的对话](codex://threads/01a0e728-773b-78c1-a163-5042273cf571) | 2026-09-28 | 1 | 全部可读轮次 |
| [查看共享对话内容](codex://threads/01a0e6e5-a318-7572-9fcd-b17011967d63) | 2026-09-28 | 1 | 全部可读轮次 |
| [检查 Query 长录制与 GPUCatalog](codex://threads/01a09ef3-6776-7173-9bb7-d8ffd26472bc) | 2026-09-28 | 6 | 部分关键轮次 |
| [梳理 N30 相关文件夹](codex://threads/01a0cd4a-7b2d-7ca3-b894-8623583b738c) | 2026-09-23 | 2 | 全部可读轮次 |
| [评估30分钟Tracy文件处理能力](codex://threads/01a0b32a-88f5-7430-93d4-676f5176ce86) | 2026-09-23 | 2 | 部分关键轮次 |
| [生成 T3 新打包工程](codex://threads/01a0c973-2637-7d00-af34-f8929cceb77a) | 2026-09-22 | 2 | 部分关键轮次 |
| [查找报告中的Tracy文件](codex://threads/01a0c3f0-1ac2-7070-9095-c8f813a6bfaa) | 2026-09-22 | 2 | 部分关键轮次 |
| [统计两个仓库分支差异](codex://threads/01a0c82c-6b96-7733-be54-6b8323771096) | 2026-09-22 | 5 | 部分关键轮次 |
| [排查 game.exe 启动崩溃](codex://threads/01a0c7fc-b356-7ba1-a7fb-936f58457312) | 2026-09-22 | 1 | 全部可读轮次 |
| [合并 Unity 与 PackageRepo 分支](codex://threads/01a0c47a-0683-7551-9e65-5bb58041abf3) | 2026-09-21 | 2 | 部分关键轮次 |
| [打开 Tracy 性能报告](codex://threads/01a0c458-f485-7551-b3ba-31d5bb461100) | 2026-09-21 | 2 | 全部可读轮次 |
| [启动 Unity 录制 Profiler 数据](codex://threads/01a0c420-8f54-71d0-b210-efe428970d3f) | 2026-09-21 | 2 | 部分关键轮次 |
| [查找 N30 Session Store 分支](codex://threads/01a0b340-ff4d-7883-9bf0-6138c5535843) | 2026-09-18 | 1 | 全部可读轮次 |
| [优化SKILL](codex://threads/01a0b2b1-ae65-71e3-8c2c-359f1193893c) | 2026-09-18 | 3 | 部分关键轮次 |
| [比较 Tracy 文件切分格式](codex://threads/01a0a836-5c32-7d31-991c-bb6022db6714) | 2026-09-16 | 1 | 全部可读轮次 |
| [查找 diagnose-jn-unity-tracy 技能](codex://threads/01a0a443-058d-7c11-96a4-9c2908771041) | 2026-09-15 | 1 | 全部可读轮次 |
| [INI文件切换](codex://threads/01a0a33b-ee90-7373-8c10-5286bb3548e2) | 2026-09-15 | 1 | 全部可读轮次 |
| [讨论 Tracy Capture GUI 设计](codex://threads/01a0a326-b197-7c12-8ec8-87b2af1d787f) | 2026-09-15 | 5 | 部分关键轮次 |
| [SKILL新版本试验 (2)](codex://threads/01a0a31e-39c3-7580-b0e3-c1dcbc8c9d71) | 2026-09-15 | 2 | 部分关键轮次 |
| [优化 Query的存储问题](codex://threads/01a09f47-1097-70f3-b3d7-d8daae7f431e) | 2026-09-15 | 5 | 部分关键轮次 |
| [查找今早的 Tracy 性能报告对话](codex://threads/01a0a077-abd0-7db1-9ff6-637473fd4a4d) | 2026-09-14 | 1 | 全部可读轮次 |
| [复制 Tracy 文件到独立目录](codex://threads/01a0a072-94e5-79f1-b6cc-ca6531804e34) | 2026-09-14 | 1 | 全部可读轮次 |
| [迭代 JN Tracy Player 技能](codex://threads/01a09f5f-eea8-7750-a3b5-94ecacf5da81) | 2026-09-14 | 3 | 全部可读轮次 |
| [说明启动游戏与 Tracy 录制](codex://threads/01a09ec8-e8d8-7240-8beb-8f6cb81c710f) | 2026-09-14 | 2 | 部分关键轮次 |
| [SKILL新版本试验](codex://threads/01a096de-d4c8-7c00-abb3-f98faf739a45) | 2026-09-14 | 5 | 部分关键轮次 |
| [部署 Tracy 诊断配置到测试机](codex://threads/01a09e96-1d9d-7412-838e-d758a49c7aca) | 2026-09-14 | 1 | 全部可读轮次 |
| [查找 Tracy 性能分析对话](codex://threads/01a0998b-a004-70b2-8d3b-53c2d1126f57) | 2026-09-13 | 1 | 全部可读轮次 |
| [查找 report-13 和原始 Tracy 文件](codex://threads/01a09627-7fa6-7e03-acf7-e9628f7006b5) | 2026-09-12 | 2 | 部分关键轮次 |
| [Unity Tracy SKILL迭代](codex://threads/01a06aa5-94ba-7853-842f-c1354d85061e) | 2026-09-12 | 3 | 部分关键轮次 |
| [Unity Tracy Query 内部分析缓存](codex://threads/01a07beb-62b4-74b2-af84-73e0de7cf53f) | 2026-09-10 | 5 | 部分关键轮次 |
| [分析 JNUnity 光追硬件开销 (3)](codex://threads/01a07aca-cbe9-75b0-a880-5cdc509fed96) | 2026-09-07 | 3 | 部分关键轮次 |
| [规划 Tracy AI 性能分析文档](codex://threads/01a07bb2-2bac-7611-bb9d-51328b969dff) | 2026-09-07 | 4 | 全部可读轮次 |
| [分析 JNUnity 光追硬件开销 (2)](codex://threads/01a07ac9-fd87-7373-a7b4-06777b7e5d5e) | 2026-09-07 | 2 | 全部可读轮次 |
| [分析 JNUnity 光追硬件开销](codex://threads/01a07a72-b7eb-71a2-ae15-3a9de617e105) | 2026-09-07 | 1 | 全部可读轮次 |
| [Unity Tracy 修复若干bug](codex://threads/01a075a3-4fa6-7be1-827b-33960910fb3d) | 2026-09-06 | 5 | 部分关键轮次 |
| [Unity Tracy Session验收](codex://threads/01a0719c-b969-70f0-8c27-525114eeec86) | 2026-09-05 | 5 | 部分关键轮次 |
| [整理Tracy Unity性能分析自评](codex://threads/01a06b1b-01a5-71e3-bd0c-982472885a99) | 2026-09-04 | 2 | 全部可读轮次 |
| [规划 Tracy 仓库学习路径](codex://threads/01a04799-c461-7893-8296-56598dcde155) | 2026-09-03 | 2 | 部分关键轮次 |
| [Unity Tracy 原始额外开销问题](codex://threads/01a0656b-fc8a-7393-894b-9b2556589f15) | 2026-09-03 | 5 | 部分关键轮次 |
| [Tracy默认开销问题](codex://threads/01a06173-6263-78e1-9f40-f6e79f25b4f9) | 2026-09-02 | 3 | 部分关键轮次 |
| [Unity Tracy 显存迭代计划 (2)](codex://threads/01a060f4-9c22-7880-b173-1ea6458b2246) | 2026-09-02 | 5 | 部分关键轮次 |
| [Unity Tracy SKILL迭代 (3)](codex://threads/01a05af4-26f3-7590-81ed-337c0167b1b5) | 2026-09-01 | 3 | 部分关键轮次 |
| [Unity Tracy SKILL迭代 (2)](codex://threads/01a056d4-65f7-7f01-82db-b7bfcdebb17a) | 2026-08-31 | 2 | 部分关键轮次 |
| [规划 Tracy 仓库上传 Git](codex://threads/01a056be-11f7-7c63-bd74-a638d78734fe) | 2026-08-31 | 2 | 部分关键轮次 |
| [打包 jntracy Player Skill](codex://threads/01a052e4-95bd-7ac0-b46a-3d0e9b99bbb5) | 2026-08-30 | 1 | 全部可读轮次 |
| [Unity Tracy SKILL迭代](codex://threads/01a04df8-afa8-7260-bcaf-ee241a473fd5) | 2026-08-29 | 5 | 部分关键轮次 |
| [Unity Tracy 配置测试机](codex://threads/01a04d7e-d2ea-7440-b5d3-22a247139bb5) | 2026-08-29 | 3 | 部分关键轮次 |
| [Unity Tracy SKILL优化 (2)](codex://threads/01a0426c-f7b0-7b03-a4a5-0a2f0cc6bab7) | 2026-08-27 | 2 | 部分关键轮次 |
| [Unity Tracy GUI迭代](codex://threads/01a032bb-52a1-7921-91b2-52446e6e93f2) | 2026-08-24 | 5 | 部分关键轮次 |
| [Unity Tracy 显存迭代计划](codex://threads/01a032a7-9ee0-7801-830e-cf69a4d82e18) | 2026-08-24 | 5 | 部分关键轮次 |
| [Unity Tracy GPU Catalog](codex://threads/01a02865-794d-79e2-9593-13766256b402) | 2026-08-22 | 5 | 部分关键轮次 |
| [Unity Tracy 测试分析 (2)](codex://threads/01a0239c-e420-7353-a236-ab124dd9ab6a) | 2026-08-21 | 2 | 部分关键轮次 |
| [Unity Tracy ResourceGraph](codex://threads/01a0224d-ecc3-73c0-8f22-0b3593dea589) | 2026-08-21 | 3 | 部分关键轮次 |
| [Unity Tracy 任务 (4)](codex://threads/01a01f3b-f5a0-7273-b56e-ae7cddd7a636) | 2026-08-20 | 2 | 部分关键轮次 |
| [Unity Tracy stream转换优化](codex://threads/01a01e85-2a38-7522-ac40-4d1f76a2a90f) | 2026-08-20 | 3 | 部分关键轮次 |
| [Unity Tracy 显存资源依赖分析](codex://threads/01a01dd5-ca62-7cd0-845d-0f2246930d2c) | 2026-08-20 | 3 | 部分关键轮次 |
| [Unity Tracy 接入迭代 (8)](codex://threads/01a01813-dbe7-74e2-a934-a260e51997cd) | 2026-08-19 | 3 | 部分关键轮次 |
| [Unity Tracy 显存资源获取](codex://threads/019fff65-5eda-7fb0-ad31-f3332e5a7507) | 2026-08-14 | 3 | 部分关键轮次 |
| [Unity Tracy 任务](codex://threads/019fff52-dbbd-7432-9d2c-9796fbe2cdfb) | 2026-08-14 | 2 | 部分关键轮次 |
| [Unity Tracy 接入迭代 (6)](codex://threads/019fff43-e0c0-7f92-8f2e-23ac217fed22) | 2026-08-14 | 3 | 部分关键轮次 |
| [Unity Tracy 接入迭代 (5)](codex://threads/019ff900-d457-7130-bb95-1a622eecc1b6) | 2026-08-13 | 2 | 部分关键轮次 |
| [Unity Tracy 接入迭代 (5)](codex://threads/019fdad7-42c6-75f3-913d-e513f2a73de0) | 2026-08-07 | 3 | 部分关键轮次 |
| [Unity Tracy 测试场景 (7)](codex://threads/019fc754-dcf6-75a3-b346-38731ba634c0) | 2026-08-03 | 2 | 部分关键轮次 |
| [Unity Tracy 测试场景 (6)](codex://threads/019fc6b6-3ee4-7351-bda5-d2ad3b4359f0) | 2026-08-03 | 2 | 部分关键轮次 |
| [Unity Tracy 接入迭代 (4)](codex://threads/019fc264-0bc1-7d42-9132-e6eff20daff3) | 2026-08-02 | 2 | 部分关键轮次 |
| [Unity Tracy 接入迭代 (3)](codex://threads/019fb779-f10c-74a2-bd54-88c17ac83276) | 2026-07-31 | 2 | 部分关键轮次 |
| [Unity Tracy 接入迭代 (2)](codex://threads/019fb779-08bd-7d03-83fe-da402f44d99b) | 2026-07-31 | 2 | 部分关键轮次 |
| [Unity Tracy 接入迭代 (2)](codex://threads/019fb335-db10-71e2-8002-317c4505cc0b) | 2026-07-30 | 2 | 部分关键轮次 |
| [Unity Tracy 接入迭代](codex://threads/019fb31e-022a-75f0-8d77-457e85b5d59b) | 2026-07-30 | 3 | 部分关键轮次 |
| [Unity Tracy 测试场景 (5)](codex://threads/019fb29a-5a37-7be2-b1aa-1d17ffed5739) | 2026-07-30 | 2 | 部分关键轮次 |
| [Unity Tracy 测试场景 (4)](codex://threads/019fb262-416a-79e3-b113-3719ca180c7c) | 2026-07-30 | 2 | 部分关键轮次 |
| [Unity Tracy 测试场景 (3)](codex://threads/019fb108-91f9-7dd3-9f55-cc69b78fc43b) | 2026-07-30 | 2 | 部分关键轮次 |
| [Unity Tracy 测试场景 (2)](codex://threads/019fb106-d8b5-7b52-b51e-2f6036a250dd) | 2026-07-30 | 2 | 部分关键轮次 |
| [Unity Tracy 测试场景](codex://threads/019fadb0-85af-7b52-9582-c0f84f4cde4c) | 2026-07-29 | 2 | 部分关键轮次 |
| [Unity Tracy 接入验收测试 (2)](codex://threads/019fad9c-3b7f-7830-a0e2-a2b9bf97dda4) | 2026-07-29 | 2 | 部分关键轮次 |
| [Unity Tracy 接入 (2) (2)](codex://threads/019fad1d-e890-74b0-bfeb-8979c125f1f0) | 2026-07-29 | 2 | 部分关键轮次 |
| [Unity Tracy 接入 (2)](codex://threads/019faca0-08cf-7402-936f-8a8041af4a5c) | 2026-07-29 | 2 | 部分关键轮次 |
| [Unity Tracy 接入验收测试](codex://threads/019fabd3-99d5-76d0-aa93-92882375b60f) | 2026-07-29 | 2 | 部分关键轮次 |
| [讨论 Unity Tracy 接入方案 (2)](codex://threads/019fa6d6-bae5-7551-8e9c-1a073692bbc8) | 2026-07-28 | 3 | 部分关键轮次 |
| [讨论 Unity Tracy 接入方案](codex://threads/019f9eb5-a8b7-7610-9bea-96fe276177bc) | 2026-07-26 | 3 | 部分关键轮次 |
| [查找Tracy流式传输对话](codex://threads/019f9205-a9ff-7453-8a70-be67ca108d21) | 2026-07-24 | 2 | 全部可读轮次 |
| [查找 Tracy 大模型性能数据 API](codex://threads/019f8928-25d0-77f2-9a29-7fca55717825) | 2026-07-22 | 1 | 全部可读轮次 |
| [查找 Tracy 中文操作文档](codex://threads/019f8880-72f6-7f21-a205-d7c0443c11dc) | 2026-07-22 | 2 | 部分关键轮次 |
| [分析 tracy 仓库扩展](codex://threads/019f887e-ce84-7801-baf5-fa863d79195f) | 2026-07-22 | 1 | 全部可读轮次 |
| [分析 Unreal Insights 流式写盘](codex://threads/019f87e8-8291-70b2-a618-782a59bc6878) | 2026-07-22 | 2 | 部分关键轮次 |
| [读取 Tracy 更改方案文档](codex://threads/019f840a-2697-7491-b931-25b1b656c7bd) | 2026-07-21 | 2 | 部分关键轮次 |
| [起草Unity接入Tracy方案](codex://threads/019f7e5a-8e33-7441-af96-d1576834045b) | 2026-07-20 | 3 | 部分关键轮次 |
| [审阅 Godot Tracy GPU 修改 (3)](codex://threads/019f7e4f-bf14-79b2-8be1-4cb5d2383070) | 2026-07-20 | 3 | 部分关键轮次 |
| [审阅 Godot Tracy GPU 修改 (2)](codex://threads/019f7e24-9c70-7721-b35b-8ca1c287a0b0) | 2026-07-20 | 2 | 部分关键轮次 |
| [审阅 Godot Tracy GPU 修改](codex://threads/019f7d96-1c2d-75e2-b162-0b60a0a5e1a3) | 2026-07-20 | 2 | 部分关键轮次 |
| [介绍 Tracy 示例功能](codex://threads/019f7d91-98d7-70a1-bcf8-e1bc562c2c4b) | 2026-07-20 | 2 | 部分关键轮次 |
| [接入D3D12 Tracy GPU分析](codex://threads/019f6ff8-7163-7c80-922b-37ed4de5a04a) | 2026-07-17 | 3 | 部分关键轮次 |
| [Locate Tracy integration](codex://threads/019f6fbd-9263-7193-9324-308ad770c4c7) | 2026-07-17 | 2 | 部分关键轮次 |
| [试验 Godot Tracy 示例 (2)](codex://threads/019f6eb1-2ce1-7f43-82fa-1292bd7911de) | 2026-07-17 | 2 | 部分关键轮次 |
| [试验 Godot Tracy 示例](codex://threads/019f6e82-2f2e-7602-ae7a-f0da6161643c) | 2026-07-17 | 2 | 部分关键轮次 |
| [集成Tracy性能分析 (2)](codex://threads/019f6e0a-7e45-7951-86d2-1bd20b85413a) | 2026-07-17 | 3 | 部分关键轮次 |
| [集成Tracy性能分析](codex://threads/019f6ad9-e469-78f3-b8f0-77b7c024c51b) | 2026-07-16 | 2 | 全部可读轮次 |
| [集成 Tracy 性能分析](codex://threads/019f6ab7-07b0-7081-9ce6-e9133128474a) | 2026-07-16 | 3 | 部分关键轮次 |
| [应用 PackageRepo patch](codex://threads/019f60ed-0148-7502-8daa-f8fdda1e7735) | 2026-07-14 | 2 | 全部可读轮次 |
| [对比 jnunity-study 文件夹](codex://threads/019f5ec1-0dd3-7b33-835c-f5b48e62f67d) | 2026-07-14 | 1 | 全部可读轮次 |

