> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 验证层级、证据包与发布判据

> 首版：2026-09-07｜命令与结论均需按适用版本阅读。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 验证分层

| 层级 | 要验证的内容 | 不能替代 |
|---|---|---|
| 静态/单元 | 局部不变量、边界、算法反例 | 真实事件覆盖 |
| 契约 | Protocol/Schema、字段含义、分页、错误状态 | 实际Player运行 |
| 合成语料 | 极值、生命周期、重连、乱序、规模结构 | 真实场景覆盖与长期稳定性 |
| 真实短Trace | 指定输入全链路、候选覆盖、输出证据 | 长Trace规模门禁 |
| 双路径差分 | 同输入、同义字段、关系和生命周期一致 | 只比公开总数或退出码 |
| Player/Editor验收 | 指定运行时和环境的有效画面/事件 | 相互替代、所有配置稳定 |
| 性能/规模 | 预设资源和时延门槛、可比多运行 | 单次快慢 |
| AI报告审计 | 数值权威、原始回执、全部强制调查 | 只检查Markdown能生成 |

分层依据 [S17](../08-references/source-index.md#s17) [S09](../08-references/source-index.md#s09) [S13](../08-references/source-index.md#s13) [S22](../08-references/source-index.md#s22)。

## 完整证据包

每次运行保留输入清单和SHA、源代码/dirty身份、工具hash、有效配置、实际命令、开始结束时间、退出码、资源采样、各阶段机器结果、原始MCP请求响应、生成报告和证据索引。原始失败不被后续修复覆盖；后续运行使用新Run ID。

报告复核至少回答：
1. 所有候选页是否完整且身份一致？
2. 强制候选是否全部调查，未完成项是否如实披露？
3. 请求/返回ID、Trace、实体和父链是否一致？
4. partial/truncated/invalid和分页状态是否处理？
5. 引用源码是否对应当次二进制？
6. 完成声明是否超过真实测试范围？

## 项目完成状态

- **Planned**：目标已记录。
- **Implemented**：有代码，未代表验收通过。
- **Unit/ContractPassed**：仅对应轻量层级通过。
- **RealTracePassedWithLimits**：指定输入/范围通过，有明确例外。
- **Failed / Incomplete**：阻断或缺失，保留原因。
- **Accepted/Released**：需要明确的交付/发布证据，不能从分支存在推断。

以上为文档中心汇总状态；原报告的 Passed_With_Declared_Exceptions、ProceedByUser、CompleteSourceDegraded 等原始值保留，不强制改名。

## 典型不能合并的结果

N30A P0/P2/P3 为不同独立Run，不拼成一轮全通过。QSCAN13 的历史必查规则不能替代QSCAN14全selected规则。N27 GUI PreviewCandidate不是已人工accepted。[S22](../08-references/source-index.md#s22) [S09](../08-references/source-index.md#s09) [S13](../08-references/source-index.md#s13) [S23](../08-references/source-index.md#s23)

## 本次文档验收

本次只做文档生成、来源存在/hash、Git只读快照和链接/结构检查；没有运行编译、游戏、Query扫描或历史性能测试。检查结果写入 [文档校验记录](../evidence/document-validation.json)。
