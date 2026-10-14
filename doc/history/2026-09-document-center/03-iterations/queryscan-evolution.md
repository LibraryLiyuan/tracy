> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# AI 工作流演进：1.x、QSCAN13 与 QSCAN14

> 首版：2026-09-07｜适用范围以来源中的阶段和版本为限。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 1.x → Query 原生扫描的原因

原有流程存在大量原始事件经 MCP 分页传给 AI/Python、多个组件重复解释统计、预算/Top/异常在报告阶段混合等问题。新设计把读取、统计和候选统一放入 Query，AI 只做定向调查和解释，Python 处理已落盘结果与报告。[S07](../08-references/source-index.md#s07)

现场安装版 SKILL metadata 为 1.1.3；草案为 2.0.1。设计文件中“当前已安装 1.1.2”是历史快照。新文档使用现场值，并保留旧设计原文。[S27](../08-references/source-index.md#s27) [S28](../08-references/source-index.md#s28)

## QSCAN 阶段分工

| 阶段 | 主题 |
|---|---|
| QSCAN2–3 | 外置 Analysis Profile、中立聚合 |
| QSCAN4–7 | 有界读取、Frame/CPU、GPU/Job/Managed、Memory/I/O/Sampling/Telemetry 扫描 |
| QSCAN8–9 | 精确统计、异常口径、候选 Policy |
| QSCAN10 | 持久异步 Scan manager 与恢复 |
| QSCAN11–12 | Skill state、报告合同 |
| QSCAN13 | 真实短 Trace、确定性重建和历史调查规则验收 |
| QSCAN14 / FW0–6 | 完整逐帧/窗口、全部 selected 必查、实际 MCP 回执与终态审计 |

阶段文件位于 [草案 docs](C:/Users/Admin/Documents/JN-Unity-T3/skill-draft/diagnose-jn-unity-tracy/docs)，分工依据 [S08](../08-references/source-index.md#s08) 及目录；QSCAN14 实际状态依据 [S10](../08-references/source-index.md#s10)。

## QSCAN13 的通过边界

历史报告记录 15 个必须调查项完成 15 个，其余 1,356 个候选保留 backlog；总体 PassedWithSourceQualityDegradation，短 Legacy Worker 验收通过，Session 由用户明确延期。它没有验收后来加强的“全部 selected 都必须调查”。[S09](../08-references/source-index.md#s09)

## QSCAN14 的实质变化

- 同名工作按真实 FrameSet、线程、完整父路径和 manifestation 绑定。
- 完整逐帧序列落盘，分别筛选帧、稳定慢窗口与全局入口。
- unknown 不当 0，Job execution 不当 critical path，显存按同时存活根分配。
- 合并窗口重新满足稳定性要求。
- 所有 selected、P1/UserFocus 进入强制队列。
- 请求/响应 ID、Trace、hash、实体/父链与范围都要审计；终态不能只信 completed ID。

[S10](../08-references/source-index.md#s10) [S13](../08-references/source-index.md#s13)

## run1历史结果与当时下一步

FW5 的 333 MiB 输入包含约 985 万 CPU Zones、96 万 GPU Zones 和 1,654 万 GPU Uses。约 22 分 39 秒后触发内存保护，cancelled_resumable、completed=false；没有完整候选发布，也没有最终安装。不能补写触发瞬间的具体峰值，因为旧 runner 没有保存。[S11](../08-references/source-index.md#s11)

已确认的修正/调查方向为临时扫描容器生命周期重叠，以及只请求 Allocation 却触发完整 GPU Snapshot 的额外工作。前者改动不等于已证明内存达标；后者不是已测定唯一根因。下一步先用有界回归确认读取范围与生命周期，再进行新的短 Trace 验收。

当前 QueryScan worktree 有 24 项 tracked 改动（只读快照，未枚举 untracked）；HEAD fd438853 不能独自代表草案最新实现。[分支说明](../04-branches/tracy.md) 给出工作区与版本关系。


## 收尾核对：run2与当前下一步

文档生成期间原进度追加run2：Catalog-only读取已修正，三个域扫描完成，但exact_statistics在扫描约19分36秒时触发保护。触发高水位约16.07 GiB，安全收尾前最终高水位约16.14 GiB；cancelled_resumable、completed=false，没有发布候选。约150万签名组数不是selected候选数。

随后修正每签名四组指标的空异常vector仍保留32槽的放大；4096签名小回归从12,582,912 bytes降到98,304 bytes，统计结果SHA不变。这不证明真实总内存达标；第三轮尚未执行。当前下一步为高基数Aggregate/Context、整体JSON序列化与恢复的有界验证，再进行新的短Trace验收。[S38](../08-references/source-index.md#s38)
