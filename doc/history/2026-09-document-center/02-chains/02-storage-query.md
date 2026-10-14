> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 链路 02：转换、持久化与 Query

> 首版：2026-09-07｜用途：开发知识与跨对话协作｜事实以标注来源和版本为限。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 三种组织方式

| 方式 | 数据流程 | 解决的问题 | 当前边界 |
|---|---|---|---|
| 传统 .tracy | Journal → 协议回放 → Worker → .tracy → Query/GUI | 通用离线读取与 GUI 复核 | 大输入全量对象与重建可能占用大量内存 |
| N29 sidecar | 转换后复用 Worker GPU 数据 → raw/derived shards → 共用 reader | 避免 GUI 首次打开时重复构建大型 GPU 分析 | 上游 replay 仍可能过大；有声明的验收例外 |
| N30 Session | Journal → 全域 Canonical shards → Mandatory Derived → Final Audit → 发布 | 将全域事实和派生读取分片化 | P3 新双路径验收失败，不能宣传全域完全等价 |

依据 [S06](../08-references/source-index.md#s06) [S19](../08-references/source-index.md#s19) [S20](../08-references/source-index.md#s20) [S21](../08-references/source-index.md#s21) [S22](../08-references/source-index.md#s22)。

## 发布与恢复的含义

N29 使用 generation、checksum、writer lease 和 checkpoint。只发布已提交 page/shard；sidecar 失败不破坏有效 .tracy。N30 的 Canonical 是转换后的事实层，Derived 应可重建；不得把临时 building 目录当完成 Session。[S19](../08-references/source-index.md#s19) [S21](../08-references/source-index.md#s21)

QueryScan 草案接受 .tracy 或已发布的 Complete / CompleteSourceDegraded Session。后者保留源质量问题，不表示所有域都适合无条件推断。[S27](../08-references/source-index.md#s27)

## 为什么需要差分验收

两个转换器 exit 0 只证明各自产生了输出。N30A P3 的 Full Worker 和 Session 转换均成功，但后续 Query 仅取得 20/24 个 Full Worker 响应，45 个计数中有 16 个不同。部分差异来自字段填充或单位，Job 底层计数仍有未解决差异。不能先豁免差异再宣称等价。[S22](../08-references/source-index.md#s22)

验证顺序应是：
1. 固定相同源输入和工具身份。
2. 分别验证源读取、转换审计和发布状态。
3. 同一查询集合比较同名同义同单位的数据。
4. 将源边界差异、API 表达差异与工具引入的数据缺失分别记录。
5. 短路径阻断未解除前，不据此启动更大验收。

## Query 读取与故障隔离

Query 返回事实、capability、coverage、quality 与分页状态；AI 不能把 partial/truncated 返回当完整集合。GPU Catalog 的源失效应限制相关分析，而不能自动污染 CPU/Job 等独立域；扫描器自身 checksum、identity 或 audit 失败则阻断本次扫描。[S12](../08-references/source-index.md#s12) [S17](../08-references/source-index.md#s17)

源码调查入口为 Tracy 的 capture、server、query 及各专题 worktree；先核对当前分支，再定位实现。默认 dev 存在 CodeGraph，代码定位应先使用索引；索引提示变动文件时按提示核对当前源码。本次索引曾提示 TracyWorker.cpp 有变动，不据旧索引输出作完整代码认证。
