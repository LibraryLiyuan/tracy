> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 近期 GPU/Render 开发的三个独立专题

> 首版：2026-09-07｜适用范围以来源中的阶段和版本为限。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## Render Thread CPU 区间修复

问题是粗阶段与 deferred Unity marker 交叉，导致 pending End 和虚假阶梯嵌套。正式方案保留 marker，在 marker Begin/End 边界关闭 stage。先前 blanket suppression 丢失 marker，已废弃。2026-09-06 专项报告通过，不覆盖 GPU/OriginFrameId。[S03](../08-references/source-index.md#s03)

## GPU L0 OriginFrameId

目标是从 Main→Render 真实交接向 CommandList Segment 传播精确 Frame ID。L0 是 Segment，不是完整 GPU Frame；主渲染/UI 多段不能仅凭时间相邻关联。已有独立交接和架构讨论，但本次没有核定最终实现与真实归属验收。[S02](../08-references/source-index.md#s02) [S37](../08-references/source-index.md#s37)

## DXR timing-only

原生 DXR 三类真实 GPU 命令加入 timestamp zone，不加入新的资源引用或硬件计数。历史报告交付提交为 18a46a79；本轮现场分支 HEAD 为 0f3f7d6f，后者包含更晚的 ENABLE_JN_TRACY helper guard 提交，因此应按实际构建 hash 识别是否包含后续变更。[S04](../08-references/source-index.md#s04) 与 [Git 快照](../evidence/repository-snapshot.json)。

本地报告先有 Player 开/关成功采集，后有同 hash Player 场景启动 device removed 0x887A0006，再有 Editor 成功 Play/采集。报告明确 Editor/Mono/诊断配置及视图尺寸不同，不能证明 Player 问题已修复或测量性能收益。

## 后续验收应分别回答

| 专题 | 核心问题 | 不可借用的证据 |
|---|---|---|
| Render CPU 层级 | marker 是否保留、树是否合法？ | GPU timestamp 正常 |
| GPU 帧归属 | Segment 是否精确绑定真实 Frame？ | 两段看起来相邻 |
| DXR | 区间是否包围实际命令且配套运行稳定？ | Editor 成功代替 Player 稳定 |
