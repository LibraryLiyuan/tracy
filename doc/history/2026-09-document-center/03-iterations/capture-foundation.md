> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 采集基础演进：N00–N25

> 首版：2026-09-07｜适用范围以来源中的阶段和版本为限。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## N00–N15：让事件可解释

早期阶段围绕 Capture Identity、Context/Producer Quality、稳定 ID、Frame Correlation、Callstack 配置和 Query partial/budget 契约，随后接入 GPU taxonomy、直接 GPU、Managed Bridge、内存、Managed/Lua、Job、I/O 和 Evidence Graph。[S16](../08-references/source-index.md#s16)

这些能力的共同目的，是让 AI 有足够信息分辨“哪里慢、属于哪个线程/帧/资源、证据是否完整”。但 2026-08-04 全量回归明确为“核心链可用，生产门禁未通过”，所以不能把索引表中的专项 Passed 当总体成熟度。

## N16：低开销与完整证据同时成立

| 子方向 | 关键设计 | 遗留边界 |
|---|---|---|
| 未连接路径 | fast gate、D3D12 lazy storage | 严格同源开销门禁独立验收 |
| Managed/Lua | 独立 source stack 与 callsite | 各运行时覆盖有条件 |
| Job Schema3 | 调度、执行、等待、继续全链 | 并发关系必须保留，不靠时序猜因果 |
| GPU/内存/I/O | 显式 Pass、registry、生命周期与重连边界 | 部分性能/完整性事项转最终矩阵 |
| Query | sidecar、分页、取消、大响应约束 | 大 Trace 时间/内存仍可能暴露问题 |

N16.12 最终生产矩阵在历史索引中为 Failed；若干子项为 ProceedByUser，即用户接受记录中的风险继续，不表示门禁已通过。N16 全开真实场景复跑为 MixedResult / ProductionGateNotPassed。[S16](../08-references/source-index.md#s16)

## N17–N24：日常工作流与 Player 正确性

本地引擎文档中已有 N17 综合验收、N18 PlayerLoop FrameRoot、N19 HighEvidence GPU Reference、N23 Player 正确性与自动化、N24 性能与 Managed 覆盖等记录。本次以历史索引和文件目录确认其存在，没有逐份重新验证最终结论；需处理具体问题时读取对应阶段原报告。

读取入口：引擎历史验收目录（历史引用当前未找到：`C:/workflow/jnunity/Runtime/Profiler/JNTracy/Docs/AI-Analysis-Iteration-Acceptance`）。将这些阶段合并成“都完成”会丢失其功能、性能、Player 和 Editor 的不同边界。

## N25：HighEvidence 热路径与采集结束

| 子阶段 | 目的 |
|---|---|
| N25.0 | 配置、协议和 telemetry cost 建立失败基线 |
| N25.1 | Sampling/Process Context Switch 的栈字典和缓存 |
| N25.2 | GPU ResourceSetV2，保留真实引用同时减少重复成本 |
| N25.3 | 可导航 callsite 和 SiteReuse |
| N25.4 | Job/CPU 热路径和阶段边界 |
| N25.5 | FrameImage 固定 staging pool，降低热路径分配 |
| N25.6 | stream-only、主动 drain、离线转换 |
| N25.7–8 | Query 复用及最终 HighEvidence 生产验收收口 |

[S16](../08-references/source-index.md#s16) 中 N25 子阶段既有 Passed，也有 ProceedByUser、性能失败及后续收口。当前采集操作采用 stream-only/正常停止的链路文档，历史阈值和提交仅作为当时版本依据。

## 给后续开发的约束

新增采集信息同时影响 producer 热路径、队列和 drain、持久化、Query 语义与性能，不能只验“事件出现了”。每次新增事件族至少检查未连接开销、连接代际、capture boundary、定义先后、三介质/路径一致性和缺失时行为。
