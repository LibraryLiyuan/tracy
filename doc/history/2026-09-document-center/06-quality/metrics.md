> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 性能指标与 AI 分析口径

> 首版：2026-09-07｜命令与结论均需按适用版本阅读。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 基本指标

| 指标 | 正确解释 | 常见误用 |
|---|---|---|
| Frame duration | 指定 FrameSet 的真实边界墙钟时间 | 把任意 marker 当完整帧 |
| CPU inclusive | 同一线程该区间及子区间成本 | 与子区间相加重复计数 |
| CPU exclusive | 依合法父树扣除子区间后的自身成本 | 跨线程相减，或父树缺失时当精确值 |
| Job execution | Job片段/工作的执行成本 | 自动等同关键路径或主线程阻塞 |
| GPU L0 duration | 指定Queue上实际Segment的GPU时间 | 当整帧GPU时间或CPU提交时间 |
| Percentile / rank | 明确总体、过滤条件、FrameSet和缺失处理的统计 | 用入选样本代替全量总体 |
| 物理显存峰值 | 同时存活、去重后的物理根分配峰值 | 历史分配总和、重复加placed子分配 |
| Pass working set | 指定Pass使用资源范围的集合 | 认为资源都独占属于该Pass |
| DXGI 与引擎计数 | 两种观测范围和时刻的指标 | 不同时间峰值相减得untracked |
| Capture/Query耗时 | 对应步骤的墙钟与资源成本 | 当作游戏某帧的CPU/GPU开销 |

当前统计修正和反例见 [S10](../08-references/source-index.md#s10) [S11](../08-references/source-index.md#s11)，GPU语义见 [S02](../08-references/source-index.md#s02) [S04](../08-references/source-index.md#s04)。

## 预算与候选

Analysis Profile 是运行时预算/策略的权威；文档中的数字仅用于解释某一历史配置。例如设计记录 Player.Frame 16.666667 ms、GPU Local 6.4 GB 十进制、Process Memory 16 GB 暂定。实际运行应读取 normalized Profile，不从本页复制为隐藏默认值。[S07](../08-references/source-index.md#s07) [S12](../08-references/source-index.md#s12)

Marker 模块预算不得直接相加为帧预算；planning_reference 不能自动触发正式预算候选。无权威预算的域可以做排名和异常，但不能冒用其他域阈值。

帧尖峰、稳定慢窗口、持续压力与全局排名是不同入口。合并两个各自稳定的窗口后，整体未必稳定，需要重新检验。Manifestation 不同，即使名字相同，也应分别取得对应时间范围证据。[S10](../08-references/source-index.md#s10) [S13](../08-references/source-index.md#s13)

## 从观测到结论

- **Observed**：存在可定位事实。
- **Supported**：事实和关系支持解释，但保留范围和反证。
- **Unresolved**：关系、能力或证据不足。
- **Root cause**：需要能解释现象的机制和证据，不能由“排名第一”直接升级。

这些词用于本中心描述证据强度，实际报告枚举以对应 Schema 为准。每个关键结论同时说明观察范围、可能混杂因素、反证、限制和最小复测。

## 性能改善如何证明

单次Trace用于诊断；采集开销或优化收益需要另立可比运行，固定场景、画质、分辨率、硬件/运行时、源码和工具、操作路径及外部负载。Editor 与 Player、Mono 与 IL2CPP、不同诊断配置不能直接当成等价 A/B。[S04](../08-references/source-index.md#s04) [S22](../08-references/source-index.md#s22)

本次文档没有重测旧报告数字；所有引用都保留原运行边界。
