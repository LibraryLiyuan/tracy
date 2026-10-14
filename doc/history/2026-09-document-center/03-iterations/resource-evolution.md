> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 资源证据演进：N26、N27、N28 与 N31

> 首版：2026-09-07｜适用范围以来源中的阶段和版本为限。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 问题如何分层

- N26/N26-R 属于资源来源链的历史探索；本地有回退/归档分支与修复报告。
- N27 建 GPU Catalog 的逻辑与物理对象、引用和 generation 闭合。
- N28 以 ResourceGraph Schema3 处理 Object/Asset/CPU/GPU 来源链、Bootstrap、生命周期和跨线程变化。
- N31 处理 Managed Catalog 的性能与生命周期成本。

它们有关联，但“Catalog 可查”“来源可追”“开销达标”是三个不同验收问题。[S17](../08-references/source-index.md#s17) [S18](../08-references/source-index.md#s18) [S05](../08-references/source-index.md#s05)

## N27：能力与未完成项

C0–C7 覆盖 ABI/协议、队列聚合、资源/分配、View/Range/RenderGraph/Mesh、VG/RTAS、Worker/stream/save-load 和 Query/MCP/INI/cost。C8 的真实 Player 与功能一致性已完成。

集中报告仍明确：Periodic60 3-pair 因场景波动 Inconclusive；RenderGraph alias 真实样本和完整动态命令流/画面哈希仍缺证据，因此不能标记生产全部 Passed。[S17](../08-references/source-index.md#s17)

N27 后续有 generation-close 修复分支；它的存在与提交摘要已写入分支快照。具体修复证据应读取引擎 Docs 中 N27-GPU-Catalog-Generation-Close-Bugfix-Acceptance.zh-CN.md，不以分支名代替验收。

## N28：统一资源图

Schema3 计划针对旧实现的事件丢失、锁竞争、未连接开销、Bootstrap 不完整、版本覆盖和乱序。关键目标是保留连接 generation、当前状态 Bootstrap 和 Delta，支持 Asset/Object/Resource/Memory/Pass 双向导航。[S18](../08-references/source-index.md#s18)

现场三仓均有 N28 分支和实现提交；本次没有逐项审计它们相对主线的行为等价，也未核定最终生产验收。把它作为独立配套实现阅读，避免拿 N27 主线工具读不兼容历史 section/schema。

## N31：已有观测与目标

计划中自动捕获 D/G 的 Frame Median 为 17.831/11.953 ms、P95 为 19.283/14.076 ms；30 秒 Journal 为 389.7/28.0 MiB。这是原计划记录的指定包与环境观测，本次未复测，不能推广为所有场景的固定损耗。[S05](../08-references/source-index.md#s05)

计划目标包括未连接 Catalog On 相对 compile-off 的 P95 增幅不超过 1%，已连接 HighEvidence 增量不超过 2%；保留真实 Pass→Resource/Range 逐帧关系，删除状态未变化的冗余更新。目标不等于已通过。

PackageRepo 当前提交含 managed compile-time switches；这能用于隔离观测，但“编译关闭后变快”不证明 Catalog 开启路径已经修复。

## 下一次文档应补什么

针对 N31 整理最终代码提交、连接/未连接矩阵、逻辑记录增长曲线、closed-window 成本、drain 结果及 GPU 语义一致性。针对 N28 补成套提交、真实资源来源双向查询、缺失/重连/乱序用例以及合并决策。
