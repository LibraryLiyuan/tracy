> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 链路 04：GPU 计时、帧归属与 DXR

> 首版：2026-09-07｜用途：开发知识与跨对话协作｜事实以标注来源和版本为限。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## L0 的语义

历史名字 GPU.Frame.Direct 表示一个 D3D12 Direct CommandList 的 GPU 执行片段，不天然等于完整 GPU 帧。CommandList 首次 Active 时打开 L0，PrepareExecute/Close 前关闭，Reset/析构等路径负责清理。[S02](../08-references/source-index.md#s02)

```text
Main → Render 帧交接
→ CommandList Active
→ L0 GPU Segment Begin
→ GPU Pass / Draw / Dispatch / Copy
→ L0 End → CommandList 提交
→ GPU timestamp 与 Segment/Pass 关系进入 Trace
```

## 精确 Frame 归属需要什么

目标关系是 GpuSegment → belongs_to_frame/exact → Frame。正确来源应来自 Main→Render 的真实交接并传播到 Segment，不能在 Render 线程临时读取 Main 当前帧号，也不能用相邻时间推断。

主渲染与 UI 可能是两个 L0；更多 Camera、Upload、Copy/Compute 可产生更多段，不硬编码“每帧两段”。没有 explicit Pass 的 L0 也需要独立归属证据。该项在本轮主要来源中仍是交接/设计；本次没有找到并复验完整落地验收。[S02](../08-references/source-index.md#s02) [S37](../08-references/source-index.md#s37)

新 AI 合同因此使用 observation_unit=l0_segment、Queue 和 representative_intervals，不把片段序号改成 Player Frame ID。[S13](../08-references/source-index.md#s13)

## DXR 第一阶段的三个观测点

| Zone | 实际包围的 GPU 工作 | 范围限制 |
|---|---|---|
| DXR.BLAS.Batch | 一批实际 BLAS Build | 不细分每个 BLAS；批次初始 barrier 在外 |
| DXR.TLAS.Build | BuildRaytracingAccelerationStructure | 实际 TLAS Build 命令 |
| DXR.DispatchRays/<RayGen> | DispatchRays | 按 RayGen entry 命名，有缓存上限和通用回退名 |

实现复用已有 GPU Zone ABI，在已有 L0 parent 下产生 timing-only 区间，不新增资源引用采集或 RT Core 硬件计数器。没有 parent 时跳过并记录 unresolved/dropped。[S04](../08-references/source-index.md#s04)

开关 JN_TRACY_DXR_GPU_TIMING 默认开启，0/false/off 关闭（后两者不区分大小写），启动设备前设置。仍服从原有原生 GPU 与连接门控，以及 PassAndPhase admission 预算。计时关闭不关闭游戏光追。[S04](../08-references/source-index.md#s04)

## 验收边界和源码入口

本地 DXR 报告记录 Player 成功开/关采集，也记录后续 device removed，reason 0x887A0006；Editor 后续取得完整 DXR 区间，不能由此认定 Player 故障已修复。Runtime/GfxDevice/d3d12/DXR/GfxDeviceDXR.cpp、D3D12CommandList.h、JNTracyD3D12Bridge 是报告列出的实现入口。

现场 DXR 分支 HEAD 已晚于原报告交付提交；[分支页](../04-branches/jnunity.md) 记录当前 HEAD 和提交摘要，但不把新 HEAD 当已完成原验收。[S04](../08-references/source-index.md#s04)
