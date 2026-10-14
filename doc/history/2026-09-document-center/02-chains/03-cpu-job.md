> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 链路 03：CPU 帧、Render 层级与 Job 因果

> 首版：2026-09-07｜用途：开发知识与跨对话协作｜事实以标注来源和版本为限。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 观察单位

CPU 分析至少需要 FrameSet、Frame 边界、实际线程、完整 Zone 父路径及 source/callsite。同名 Marker 出现在不同父路径时属于不同上下文；不能只按名字合并后声称完成调查。[S10](../08-references/source-index.md#s10) [S13](../08-references/source-index.md#s13)

```text
Player Frame
├─ Main Thread：工作 / 等待 / 有意限帧
├─ Render Thread：命令回放与 CPU 区间
└─ Job：Schedule → Ready/Queue → 执行片段 → Complete
                                      ↓
                         WaitEnd → Continuation
```

上图表示需要验证的关联结构，不表示所有线程都严格落在同一 Player 帧边界内。跨线程关联应使用真实 ID/关系及边界信息。历史 Job Schema3 链路和 capture boundary 记录见 [S16](../08-references/source-index.md#s16)。

## Render 嵌套缺陷与修复

旧 direct stage 只在 GfxCommand 分类变化时切换，Unity deferred marker 可能在 stage 中打开、跨 stage 关闭。CPU Zone 必须遵守 LIFO；pending end 机制会把交叉区间显示成虚假阶梯。

正式修复是在每个 deferred marker Begin/End 前关闭当前 direct stage，保留 Unity marker 和设备调用顺序；下一个可分类 GfxCommand 再打开 stage。曾经“抑制 marker”的方案已废弃，因为会丢失真实 Render 信息。[S03](../08-references/source-index.md#s03)

```text
关闭 direct stage
→ 原 Unity profiler/device marker 边界
→ 下一条 GfxCommand 重新开启适当 stage
```

验收：direct stage 祖先链中不能再有另一 direct stage；Frame Root 不出现 crossing/nesting/duplicate root。此项只验 CPU Render 结构，不验 GPU OriginFrameId。

## 从热点到原因

1. 确认帧慢在 Active Work、Wait 还是 Intentional Pacing。
2. 展开实际线程和完整父树；inclusive 用于范围，exclusive 用于同线程自身成本。
3. 遇到 Job 等待，沿 Schedule/Complete/Wait/Continuation 关系定位实际阻塞工作。
4. Sampling 和源码用于解释执行内容，不能代替缺失的候选事件树。
5. 将关键路径未证明、边界缺失和仅有时间重叠写成限制。

Job execution 的总和或最大值不是 critical path；多个 Worker 可以并发执行。QSCAN14 已为相关统计错误增加定向回归，但真实完整验收仍未通过。[S10](../08-references/source-index.md#s10) [S13](../08-references/source-index.md#s13)

## 源码与证据入口

- 引擎 Runtime/GfxDevice/threaded/GfxDeviceWorker.cpp：marker 边界与粗阶段。
- Runtime/Profiler/JNTracyProfilerBridge.h：Frame Root API/stub。
- Runtime/Misc/PlayerLoop.cpp：帧根与交接背景。
- QueryScan 实施进度与原始 MCP 回执：验证父链、FrameSet 和线程身份。

这些位置由 [S02](../08-references/source-index.md#s02) [S03](../08-references/source-index.md#s03) 指明；具体行号随分支变化，修改前重新定位。
