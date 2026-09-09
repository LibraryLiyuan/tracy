# Query 内部分析缓存分块化实施计划

日期：2026-09-07。用户已批准实施；当前不是完成报告。

## 1. 范围和基线

- 分支：`feature/JNTracy-QueryAnalysisCache`。
- Worktree：`C:\CodeProjects\GodotProjects\tracy-0.13.1-query-analysis-cache`。
- 来源：`feature/JNTracy-AI-QueryScan-DevBase`，HEAD `0cb512521481501f1aeff72af5a73b445a26dad2`。
- 此基线已有独立 `TracyAnalysisExternalSort` / `TracyAnalysisWriterLease`，不引入 Session Store。
- 仅在此 worktree 修改 Query/analysis、相关测试、构建和文档。不切换/合并/推送 dev，不修改其他 worktree。
- 不修改 Unity、PackageRepo、Player、Capture、Converter、Profiler、录制协议或原始 Trace。
- 测试/构建产物使用本 worktree 内 `build-query-analysis-cache*`；已有依赖源只读复用，不写其他 worktree。

输入仍为 legacy `.tracy`。这是可重建的分析结果缓存，不是新的 Trace 介质；不接入 N30 验收。

## 2. 已批准的要求

1. 首次扫描、候选生成、缓存重开及 MCP 摘要/分页/单候选查询均须有界，不能一处落盘、下一处全量读回。
2. 中立统计与候选策略分开；Profile 改变只重算候选，不重新扫描原 Trace。
3. 帧、窗口、全局候选三路语义不变；不减少数据域、不截断层级、不提高阈值、不隐藏 selected。
4. 所有 selected 仍需 AI 调查；Capture 质量仍为独立附录。
5. 新增分析工作区目标 2 GiB、预算上限 4 GiB；整个 Query 进程验收上限 32 GiB（2026-09-09 用户明确从 16 GiB 放宽）。Worker 原始加载占用计入总量，但不在本轮改造成按需 Worker。CPU/GPU 业务容量阈值不随进程保护上限改变。
6. 内存不足时明确取消/暂停并保存可信阶段，不以丢数据降级。进程采样保护不能冒充操作系统绝对分配上限。
7. 保持既有 MCP 操作和主要响应字段；内部缓存 schema/identity 独立升级，旧 cache 不冒充新格式。
8. 只承诺经验证的阶段/块边界恢复；未完成 `.work` 缺少字典和身份时不能直接当作可恢复结果。

## 3. 三层模型

| 层 | 内容 | 失效条件 |
|---|---|---|
| Neutral | Signature/Context、完整帧序列、精确统计、排名、源质量 | Trace 强身份、构建/统计算法或存储 schema 改变 |
| Policy | 入选候选、三路来源、代表实例、排序和分页索引 | Neutral identity 或规范化 Profile 改变 |
| Investigation | MCP 收据、调查结果、未完成队列 | Skill 管理；候选改变后重新验证证据绑定 |

内部使用紧凑、有大小限制的记录块和索引；允许单记录按确定性 JSON 编码，但禁止构造所有记录的 JSON DOM/string。JSON 仅在当前记录、当前响应和小型 manifest 范围内构建。

字符串/路径用字典和稳定 ID 关联；完整调用路径必须可还原。不得仅凭名字合并不同来源，不使用概率近似身份或随机抽样。

## 4. 存储及读取约束

- Writer 按已定义顺序输出；块包含 schema、计数、大小及校验值，发布前校验。
- 数据块不可变；完成 manifest 最后原子发布。未完成缓存只允许查构建状态。
- ID/ordinal 索引支持精确定位；范围/分页读取不构造全表。
- 块大小、响应大小、索引热页使用统一工作区预算，而非各模块各占 4 GiB。
- 写后校验和重开校验以固定缓冲顺序读取，不同时保留原始巨型 payload 和读回副本。
- 同 identity 的只读者固定同一 generation；不覆盖已有有效 generation。
- 取消、缺块、校验失败、重复关键身份、错误版本均有明确错误；不能以空数组表示成功。
- 跨块代表实例/窗口保持完整。缓存只控制存储规模，不控制 selected 数量。

## 5. 阶段和文件映射

| 阶段 | 工作 | 门禁 |
|---|---|---|
| AC0 | 签名身份/数量诊断，基线、Oracle 与规模测试 | 重复实例不误变稳定签名；不同路径/线程不误合并 |
| AC1 | 结果块、Reader/Writer、索引、字典、校验 | 多块 roundtrip、随机定位、分页、corruption/identity 拒绝、缓冲有界 |
| AC2 | `TracyExactStatistics` / `AnalysisScanProducts` 逐批结果输出与排名归并 | 小样本全字段一致，消除全量结果 JSON 往返 |
| AC3 | `TracyCandidatePolicy` / `TracyFrameWindowPolicy` 分批输入输出 | 三路候选集合/理由/代表实例一致；避免反复线性关联 |
| AC4 | `TracyAnalysisScanManager` 的 summary/signatures/candidates/get/representatives | 读一页不加载全表；复用缓存、单项查询与原契约一致 |
| AC5 | 工作区预算、阶段进度、取消、可信恢复和 generation | 超预算不发布；取消/重开不重复、不漏项 |
| AC6 | 新 Query 与常驻 MCP、短 Trace、Skill 调查入口验收 | 总内存通过、自然检出已知遗漏、质量独立附录、报告记录 |

主要修改既有 `TracyNeutralAggregateStore`、`TracyExactStatistics`、`TracyCandidatePolicy`、`TracyAnalysisScanManager`；新存储组件须小而独立，不变成通用数据库。不因公共 helper 来源而要求 Session 全域验收。

## 6. 正确性与测试策略

先小型红绿回归，再高基数合成测试，最后只做必要的真实短 Trace 集成测试。

- 精确统计：分位数、unknown、occurrence、FrameSet 分母、排名/并列、父路径、不同线程不变。
- 候选：单峰/双峰、稳定偏慢、局部 Top、绝对成本、正常参照、三路并集和不同表现。
- 存储：边界记录、空表、跨块页、同名不同键、缺块/截断/错误 checksum、版本/强身份不符。
- 重开和恢复：不能只测试写入；首次冷读、后续分页、取消后重开都要记录工作区与进程内存。
- Signature 规模分级增长到接近 1,506,514 组；先测存储组件，再测统计/Policy 全链。不得用原始 Trace 反复长跑代替定位。
- 小样本使用旧全内存路径作为数值 Oracle，不作为大输入的静默回退。

真实输入：

`C:\Users\Admin\Documents\JN-Unity-T3\QSCAN13-Short-Skill-Acceptance-20260907\oracle-existing\GPUCatalogOff-HighEvidence-20260903-202313.tracy`

SHA-256：`da5495e1bc5d85962bb0f9bdb2cabdc41e35c51ea0f1576a9eab702f36c15211`。

真实验收使用 `user_focus=[]`，不得硬编码 396/397 或 Marker 名。验收后核对 396/397 局部 Render/Main 热点、290/400/540 附近不同压力表现是否自然入选。它们是测试用例，不是筛选规则。

## 7. 历史证据及完成边界

旧分支 run2 在 exact_statistics 阶段达到 16 GiB 保护；临时合并输出包含 16,056,482 条记录、1,506,514 组签名，单组最多 5309 条。签名组数不等于 selected 数。

空异常 vector 的 32 槽长期预留已修正并存在于新基线；4096 签名测试保留容量由 12 MiB 降为 96 KiB、统计 hash 相同。但这不能证明全量结果、JSON、Policy 或 MCP 内存已通过。

本计划只在 AC0–AC6 均有证据后称完整完成。存储单元测试通过不等于 Query 接入；Query 扫描通过不等于全部 AI 调查完成。阶段失败保留证据及具体下一步，不自动再次发起无变化全量重试。
