> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 能力、条件与限制矩阵

> 首版：2026-09-07｜用途：开发知识与跨对话协作｜事实以标注来源和版本为限。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

本表区分“系统存在这类能力”和“任意 Trace 都可作此分析”。域是否可用，以该 Trace 的 capability、coverage、quality、validation 为准。

| 能力域 | 可用证据与用途 | 必须满足的条件 | 不可直接推出的结论 |
|---|---|---|---|
| CPU Frame / Zone | 帧分布、热点层级、同线程 inclusive/exclusive | 权威 FrameSet、合法父树、完整边界 | 跨线程耗时简单求和等于帧长 |
| Render Thread | 粗阶段及真实 Unity marker | 使用保留 marker 的边界分段修复版本 | Render 修复意味着 GPU 帧归属已修复 |
| Job | Schedule、Ready/Queue、执行片段、Complete、Wait/Continuation | Job generation 和关系完整 | 最长 Job 或总 execution 就是 critical path |
| GPU Zone / Pass | GPU timestamp、Queue、Pass/Segment 关系 | 时间区间完整、队列和父级可信 | 两段相邻 L0 必属于同一 Player 帧 |
| DXR 时间 | BLAS batch、TLAS build、DispatchRays 区间 | 配套 DXR 分支、门控有效、已有 L0 parent | RT Core 利用率、光线吞吐量、独立 inline RayQuery 耗时 |
| GPU Catalog | Resource/Allocation/View/Logical/Range、Pass 引用 | Catalog generation 有效，生命周期及引用完整 | 资源大小之和就是物理显存 |
| ResourceGraph | Asset/Object/资源/CPU 与 GPU 内存来源链 | 使用 N28 等配套实现且该关系确实采集 | N27 Catalog 已完整覆盖 Unity Asset 来源 |
| Sampling / Context Switch | 热点采样及调度等待辅助证据 | Trace 实际 capability；权限仅是前置条件 | 管理员启动就一定有有效系统事件 |
| Managed / Lua | 运行时区间、来源栈、GC 等 | 采集配置、编译和运行时覆盖有效 | 缺少事件表示不存在开销 |
| I/O | 请求生命周期与关联来源 | capture boundary、终态和关系完整 | 所有 I/O 都阻塞了主线程 |
| FrameImage | 对照场景画面与帧 | 图像对应帧及有效画面源 | 一张正确截图证明零渲染语义影响 |
| Query 原生扫描 | 确定性聚合、策略候选、分页与恢复 | Query 1.35 + 对应 Profile/草案 | QSCAN13 通过代表 QSCAN14 已验收 |
| N30 Session | 全域分片、派生索引、低内存读取 | 已发布状态、审计、版本配套 | 单次 24/24 响应等于所有路径语义等价 |

证据：CPU/Job/系统域 [S16](../08-references/source-index.md#s16)；Render [S03](../08-references/source-index.md#s03)；GPU 归属 [S02](../08-references/source-index.md#s02)；DXR [S04](../08-references/source-index.md#s04)；Catalog/ResourceGraph [S17](../08-references/source-index.md#s17) [S18](../08-references/source-index.md#s18)；QueryScan [S09](../08-references/source-index.md#s09) [S10](../08-references/source-index.md#s10)；Session [S21](../08-references/source-index.md#s21) [S22](../08-references/source-index.md#s22)。

## 状态判定

- **present=false / unavailable**：缺失域；说明影响范围，不能按零开销处理。
- **invalid / partial / truncated**：存在数据但不足以支持完整结论；保留原因。
- **源数据降级**：可以保留其他有效域分析，不得声称已修复原始数据。
- **转换/扫描身份、checksum 或审计失败**：工具可信性问题，不能当作普通源质量附注放行。
- **已实现但未真实验收**：仅描述代码能力和已有测试，不标记可交付。

Scope、单位和证据范围必须随每个结论保存。[S12](../08-references/source-index.md#s12) [S13](../08-references/source-index.md#s13)
