> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 链路 06：Query 原生扫描到 AI 证据报告

> 首版：2026-09-07｜用途：开发知识与跨对话协作｜事实以标注来源和版本为限。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 新版主路径

```text
输入与能力预检
→ 常驻 Query 1.35 MCP
→ Profile 校验与 Trace ready
→ 全量确定性扫描
→ Neutral Aggregate
→ 配置化 Policy
→ 完整 Candidate Manifest
→ 强制候选逐项 MCP 调查与源码核查
→ 原始回执/范围绑定审计
→ 报告与终态验证
```

适用开发草案 2.0.1。已安装 Skill 1.1.3 仍是另一配置/工作流入口。[S27](../08-references/source-index.md#s27) [S28](../08-references/source-index.md#s28)

## Query 和 AI 的边界

Query 读取 Trace、建立树、计算统计、生成候选；AI 不重算 percentile、exclusive、峰值、排名或触发理由。AI 负责解释、建立和排除假设、核对源码和提出复测方向。Python 只处理状态、已落盘响应和报告渲染，不能成为全量事件扫描的第二套权威。[S07](../08-references/source-index.md#s07)

## 每一步的退出证据

| 步骤 | 必要证据 | 不能接受的替代 |
|---|---|---|
| MCP 预检 | EXE SHA、initialize、tools/list、describe/schema | 目录名含 latest |
| Profile/Trace | normalized Profile、输入强身份、ready、域质量 | 用户文本描述代替 effective config |
| 扫描 | scan_id、进度、completed=true、Complete | 固定等待足够久 |
| 候选分页 | 每页原始响应、cursor、总数与 content identity | 只看前一页/Top N |
| 调查 | 实际候选范围、线程/Queue、父链、原始请求响应 | 仅源码解释或同名无关 Zone |
| 报告 | findings/evidence/candidates 一致，回执 audit | 只填 completed ID 或改报告文件名 |

[S12](../08-references/source-index.md#s12) [S13](../08-references/source-index.md#s13) [S15](../08-references/source-index.md#s15)

## 调查覆盖与状态

当前草案要求 selected=true 的全部候选、P1 及 user_focus 候选进入强制调查，优先级只决定顺序。未入选且非强制项才可保留可选 Backlog。

阶段报告须显式 in_progress，并保留全部未查项。完整报告不得有待查 selected；无法取得证据时记录 EvidenceGap 和 Unresolved，不伪造成 Supported。每个定向证据目标最多按合同尝试三次合理缩范围/分页查询，不靠无限重试耗尽资源。[S13](../08-references/source-index.md#s13) [S12](../08-references/source-index.md#s12)

## 恢复

保存 Trace/Query/Profile/算法身份、scan_id、原始分页、ledger、调查队列和 audit。CancelledResumable 需要由状态回执证明；身份及算法变动后不能盲目复用旧候选或旧 scan。相同可验证中立聚合可能复用，但 Policy 改变必须重评估。报告验证成功后再 close scan/Trace。

## 当前未闭环项

QSCAN13 的历史短 Trace 通过不能覆盖 QSCAN14 加强后的全部 selected、窗口稳定性和原始 MCP 回执要求。FW5 run1约22分39秒后因内存保护取消；后续run2完成三个域扫描，但精确统计阶段约19分36秒触发16 GiB保护。Catalog-only与空异常vector容量已修正，第三轮未执行，不能宣称真实总内存达标。[S38](../08-references/source-index.md#s38)[S09](../08-references/source-index.md#s09) [S10](../08-references/source-index.md#s10) [S11](../08-references/source-index.md#s11)

