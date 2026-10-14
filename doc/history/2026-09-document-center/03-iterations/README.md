> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 开发迭代总览与阅读顺序

> 首版：2026-09-07｜适用范围以来源中的阶段和版本为限。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 演进主线

| 阶段 | 为什么开展 | 形成的能力/方向 | 首版判定与阅读入口 |
|---|---|---|---|
| N00–N15 | 补足 AI 可消费的采集身份与多域事实 | Identity、Quality、稳定 ID、GPU、Managed、Job、I/O、Evidence | 专项 Passed 不覆盖后续全量生产失败 |
| N16–N17 | 提升证据完整性、采集治理与查询可扩展性 | Fast Gate、来源栈、Job Schema3、sidecar/MCP 等 | 见 [采集基础迭代](capture-foundation.md) |
| N18–N25 | 修复帧根、Player 正确性并优化 HighEvidence | FrameRoot、采样、资源集合、Callsite、FrameImage、stream-only | 各专题与最终门禁分别记录 |
| N26/N26-R | 建立并修复资源来源链 | 历史 ResourceGraph 探索与回退记录 | 不等于现行 Catalog 合同 |
| N27 | 建立 GPU Resource Catalog | Resource/Allocation/View/Range/Pass 证据 | 功能通过、完整生产门禁仍有例外 |
| N28 | 重建统一来源图 | Schema3、Bootstrap/Delta、版本与双向关系 | 三仓分支存在；本次未找到充分最终验收 |
| N29 | 解决大型 GPU 分析反复构建 | 持久化 GPU sidecar、Builder、reader | 有声明例外的通过 |
| N30/N30A | 解决全域长 Trace 规模与可信发布 | Canonical Session、Derived、Audit | 历史 A7 与新 P3 失败必须分开 |
| N31 | 降低 Managed Catalog 开销 | 生命周期修复、重复事件抑制、成本预算 | 计划与历史测量，未核定最终完成 |
| Render / GPU Origin / DXR | 修复层级并补足 GPU 计时语义 | CPU marker 分段、帧归属设计、原生 DXR 区间 | 三个独立专题，不互相替代验收 |
| QSCAN13→14 | 将统计收归 Query 并加强调查覆盖 | 确定性候选、窗口、完整 MCP 回执 | 旧阶段通过；新真实验收未通过 |

N00–N25 依据 [S16](../08-references/source-index.md#s16)。资源演进见 [资源专题](resource-evolution.md)，存储演进见 [N29/N30A](storage-evolution.md)，AI 演进见 [QueryScan](queryscan-evolution.md)，近期 GPU 工作见 [Render/GPU/DXR](gpu-evolution.md)。

## 如何理解“完成”

实施计划定义目标；阶段报告记录当时证据；后续回归可能发现新的失败。文档应追加失败记录并限定旧通过范围，不能改写旧证据，也不能把旧 Passed 自动继承到新规则。

每轮迭代最小记录为：问题 → 基线 → 方案 → 实际改动 → 成果 → 测试证据 → 例外 → 后继任务。跨仓迭代还需要配套分支和协议矩阵。

首版重点是把现有关键演进串起来；没有逐个重跑 N00–N31 的历史测试，也不把只有标题的资料写成已验证结果。
