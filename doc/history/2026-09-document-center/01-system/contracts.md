> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 核心对象、身份与接口契约

> 首版：2026-09-07｜用途：开发知识与跨对话协作｜事实以标注来源和版本为限。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 对象关系

| 对象 | 含义 | 关联时必须保留的信息 |
|---|---|---|
| Trace | 单次捕获或发布后的 Session 输入 | 路径、内容 SHA、工具身份、捕获环境 |
| FrameSet / Frame | 特定帧集合中的边界与标识 | FrameSet、Frame ID、起止时间；不能混用序号 |
| CPU Zone | 某线程的嵌套执行区间 | 线程、完整父路径、source/callsite、边界完整性 |
| Job | 可调度工作及生命周期 | 稳定 ID/generation、阶段、依赖、等待与继续关系 |
| GPU Segment | CommandList 的 GPU 执行区间 | Queue、Segment ID、时间、父层级；精确 Frame 关系另验 |
| GPU Pass | 语义渲染工作及 GPU/资源关系 | Pass ID、来源、所属 Segment、有效引用集合 |
| Logical / Gfx Resource | 逻辑语义资源与原生资源身份 | generation、定义版本、生命周期、映射有效期 |
| Allocation / Heap / Range | 实际分配及区域 | 根分配、子分配、同时存活范围、所有权 |
| Candidate | Query 策略产生的调查入口 | ID、触发原因、指标、范围、代表实例、策略身份 |
| Finding / Evidence | AI 结论与可复核依据 | 原始 MCP 请求/响应、hash、实体绑定、源码版本、限制 |

GPU / Catalog / ResourceGraph 概念分别来自 [S02](../08-references/source-index.md#s02) [S17](../08-references/source-index.md#s17) [S18](../08-references/source-index.md#s18)；候选与证据来自 [S12](../08-references/source-index.md#s12) [S13](../08-references/source-index.md#s13)。此表是语义词典，字段的精确拼写以对应版本 Schema 和 MCP tools/list 为准。

## 不能省略的身份

一次可复现运行至少绑定：

```text
Run ID
+ 三仓 commit / 已使用 dirty patch
+ Query/Capture/Converter/Profiler 文件 SHA
+ Player DLL/PDB 与部署 manifest
+ effective capture config
+ 输入 Trace SHA / Session generation
+ normalized Profile SHA
+ scan / aggregate / policy identity
+ 结论所引用的 MCP 请求与响应 SHA
```

只记分支名不够：分支会移动；只记 HEAD 也不够：未提交修改会影响二进制。当前 Git 状态见 [快照](../evidence/repository-snapshot.json)。二进制不能仅凭所在目录推定版本。

## 版本不是一个数字

ABI、Tracy Protocol、JN section、Query schema、Query index、GPU Catalog schema、Session/Derived schema、Scan algorithm 和 Skill version 分别约束不同层次。

| 历史/开发合同 | 已有记录 | 使用边界 |
|---|---|---|
| N27 / 旧部署 | ABI 1.7、Protocol 90、section 12、Query 1.32、Index 13、Catalog 1 | 该轮合同，非统一最新版本 |
| N29 | Protocol 90、section 12、Query 1.33、GPU analysis schemas 1 | N29 sidecar 路径 |
| N30A | P2 独立运行记录 Query 1.34.0 | P3 仍失败，不等于完整验收 |
| QueryScan 草案 | Query schema 精确 1.35.0，草案 Skill 2.0.1 | 本轮现场读取；安装版仍 1.1.3 |

来源 [S17](../08-references/source-index.md#s17) [S19](../08-references/source-index.md#s19) [S22](../08-references/source-index.md#s22) [S27](../08-references/source-index.md#s27) [S28](../08-references/source-index.md#s28)。本次未运行 EXE 版本检查，因此表内 Query 版本为文档合同，不是对磁盘全部二进制的现场认证。

## 时间、边界与失败值

- ns 与 ms 在展示前明确换算；容量同时说明 bytes、十进制 GB 或二进制 GiB。
- CPU duration、GPU timestamp 和采集墙钟耗时有不同含义。
- inclusive 包含子区间；exclusive 只在同一合法树与线程范围内解释。
- GPU L0 片段序号不是 Player Frame ID。
- 未完成生命周期不能默认补一个结束时间。
- unknown/null/unavailable 不转换为 0；查询失败不转换为空集合。
- 不同源、不同时间或不同统计口径的数值不可相减生成“未跟踪开销”。

具体统计规则见 [指标口径](../06-quality/metrics.md)。
