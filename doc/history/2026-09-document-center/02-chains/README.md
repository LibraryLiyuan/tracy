> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 关键链路阅读地图

> 首版：2026-09-07｜用途：开发知识与跨对话协作｜事实以标注来源和版本为限。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

| 链路 | 回答的问题 | 主要变更关联 |
|---|---|---|
| [01 采集与落盘](01-capture.md) | 事件怎样进入可验证文件，怎样正常结束？ | N25.6、Capture/INI |
| [02 存储、转换与查询](02-storage-query.md) | .tracy、N29 sidecar、N30 Session 有何区别？ | StreamConvert、N29、N30A |
| [03 CPU、Render 与 Job](03-cpu-job.md) | 帧、线程、区间和等待怎样建立关系？ | N16/N17/N25、Render 修复 |
| [04 GPU、帧归属与 DXR](04-gpu-frame-dxr.md) | GPU 时间属于什么工作，能否绑定 Player 帧？ | OriginFrameId、DXR |
| [05 资源与显存](05-resource-memory.md) | 从 GPU Pass 怎样追到资源、分配与来源？ | N26/N27/N28/N31 |
| [06 AI 分析闭环](06-ai-analysis.md) | 候选怎样成为有证据的结论并支持恢复？ | QSCAN、Skill 2.x |
| [07 构建与交付](07-build-delivery.md) | 源码怎样成为可验收、可复现的 Player/工具？ | 三仓、staging |

这些文档综合现有设计和验收记录，不宣称已逐函数审计全部当前源码。每条链路都标明事实依据、失效方式和下一步核验入口。
