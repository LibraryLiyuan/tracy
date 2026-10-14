> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 存储查询演进：StreamConvert、N29、N30 与 N30A

> 首版：2026-09-07｜适用范围以来源中的阶段和版本为限。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 离线转换：先移除重复工作

历史转换优化中，JnGpuReferenceSetUse 每次 reserve(current_size + set_size) 引发反复搬迁，形成近似二次构建成本。专项修复限制在 OfflineConvert 路径，以几何增长保留事件顺序与字段语义。阶段报告同时强调：快了仍须做语义比较、取消/资源保护、Release 重复测量和语料门禁。[S24](../08-references/source-index.md#s24)

## N29：GPU 分析从临时对象变成持久产物

N29 在转换后复用 Worker 数据生成 sidecar，Builder 以 shard/page/generation、checksum 和 lease 支持取消恢复，GUI 与 Query 共用 reader。验收状态为 Passed_With_Declared_Exceptions。

声明的例外包括：没有真实 60 分钟 gameplay capture；锁屏导致最后 GUI 点击交互未做；Builder 大 generation 内不支持细粒度抢占式重排。不能把 synthetic 一小时等价语料写成真实一小时录制。[S19](../08-references/source-index.md#s19)

## N30：解决全域规模

真实 30 分钟 .tracy-stream 约 19.12 GB 的历史转换在 42.6% 时 process commit 达 40.83 GiB 并触发保护，推动全域 Canonical Session 方案。N30 从 N29 的 15ceef12 基线独立 worktree 开始。[S20](../08-references/source-index.md#s20)

N30/N30A 要求事实层 Canonical、可重建 Derived、发布前 Final Audit；GPU 派生构建本身也必须分片和有界，不能只是把 GUI 中的全量构建搬到转换阶段。[S06](../08-references/source-index.md#s06) [S21](../08-references/source-index.md#s21)

## N30A：两个不同时间的结论

| 记录 | 实际含义 |
|---|---|
| 2026-09-02 A7 | 已有一次 30m Session 发布；CompleteSourceDegraded，独立 GPU verifier mismatch=0、Release Query 24/24 |
| 2026-09-07 P3 | 新短 Trace 双路径验收失败；Full Worker Query 不完整，45 项计数中 16 项不同；停止依赖它的新 30m |

[S21](../08-references/source-index.md#s21) [S22](../08-references/source-index.md#s22)。两次使用不同运行身份和更具体的门禁，不应把旧 A7 删除，也不能用旧 A7 抵消新 P3 失败。

P3 的 Session Query 在单次观测中明显更快、内存更小，但两个转换的工作范围不等同，并存在外部任务影响。不能把单次时间比值当正式性能门禁或线性预测 30m 成本。

## 下一步进入条件

先解决短 Trace Query 完整响应与字段语义/底层计数差异，保存原因、修复和独立差分证据，再重新执行依赖阶段。Profiler Session 直开和局部 .tracy 导出在计划中是后继独立里程碑，不应从“Session 可查询”推定已经交付。
