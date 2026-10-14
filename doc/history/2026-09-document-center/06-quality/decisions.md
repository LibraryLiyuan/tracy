> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 关键架构决策记录

> 首版：2026-09-07｜命令与结论均需按适用版本阅读。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

下列为既有资料中决策的整理，不是本轮新批准的代码设计。每项保留适用分支与证据；后续推翻时另写新决策并链接原项。

| ID | 决策 | 原因与代价 | 状态/来源 |
|---|---|---|---|
| D01 | 正式实时采集stream-only，离线转换 | 降低实时双写/重建负担；需要完整drain与转换校验 | 现有采集合同 [S14](../08-references/source-index.md#s14) |
| D02 | QueryScan统计、聚合、候选由Query拥有 | 避免AI/Python重复扫描和语义漂移；Query必须承担规模/恢复治理 | 新架构，真实QSCAN14未闭环 [S07](../08-references/source-index.md#s07) |
| D03 | 中立聚合与Policy分开 | 改预算可重评估，避免重扫；缓存身份更严格 | 新扫描合同 [S12](../08-references/source-index.md#s12) |
| D04 | 全selected及强制项必须调查 | 防止用优先级跳过已入选问题；调查量可能增加 | QSCAN14草案 [S13](../08-references/source-index.md#s13) |
| D05 | GPU L0是Segment，精确Frame需真实传播 | 防止错帧和虚构GPU总帧；暂缺关系时只能分析区间 | 交接目标/调查边界 [S02](../08-references/source-index.md#s02) |
| D06 | 保留Unity Render marker，在边界分段stage | 消除交叉嵌套同时保留信息；粗阶段变成多个合法片段 | 专项通过 [S03](../08-references/source-index.md#s03) |
| D07 | N29持久GPU sidecar，N30全域Canonical+Derived | 避免重复全量内存构建；增加schema、hash、lease、audit复杂度 | N29例外通过/N30A新P3失败 [S19](../08-references/source-index.md#s19) [S22](../08-references/source-index.md#s22) |
| D08 | Catalog与资源来源图分层 | GPU逻辑物理关系和Asset/Object来源有不同生命周期与成本 | N27/N28设计 [S17](../08-references/source-index.md#s17) [S18](../08-references/source-index.md#s18) |
| D09 | DXR第一阶段timing-only | 对真实命令测时，限定新增开销与语义；不提供RT硬件利用率 | 具体运行验收边界 [S04](../08-references/source-index.md#s04) |
| D10 | Player使用完整独立副本和确定替换集 | 保留资源/插件/配置与可回退证据；需要复制成本和静态验证 | staging合同 [S29](../08-references/source-index.md#s29) |
| D11 | 源质量降级与工具审计失败分开 | 保留其他有效域，同时防止工具数据缺失被掩盖 | Session/扫描合同 [S21](../08-references/source-index.md#s21) [S12](../08-references/source-index.md#s12) |

## 再次评估的触发条件

新增Graphics Jobs/Async Compute平台、协议/Schema变更、跨帧GPU归属落地、N31性能达标、N30差分失败修复、Skill正式安装发布时，应检查相关决策是否仍适用。性能目标变动应改Profile和对应决策，而不是改旧报告数字。
