# 开发演进 历史决策与分支关系

本页保留“为什么走到现在”的脉络。历史阶段的 Passed、Partial、Failed 和 ProceedByUser 只属于当时范围；当前运行入口以本目录的 00 至 14 章为准。

## 演进主线

| 阶段 | 目标和形成的能力 | 需要保留的边界 |
|---|---|---|
| 7 月原型 | Tracy 原理、Godot D3D12、GPU/FrameImage、结构化导出 | 原型环境与 Unity 生产包不同 |
| 流式纵向链 | Journal、提交尾、恢复、离线转换、MCP | 最初小样本通过不证明所有数据规模 |
| Unity 初始接入 | 单 DLL、Profiler 转发、Job、Queue/GPU、Managed、Editor/IL2CPP Player | GUI、复杂图形矩阵和生产开销分阶段 |
| N00–N16 | 身份、质量、稳定 ID、SourceStack、Job Schema、I/O、Evidence、低开销 | 专项功能通过与严格生产性能门槛不同 |
| N17–N25 | 配置档位、帧根、自动登录、Player 正确性、资源集合、Callsite、FrameImage、stream-only | N20/N21 登录结论作废；部分性能门槛保留失败或待验 |
| N26–N28 | ResourceGraph、GPU Catalog、双向资源来源和生命周期 | 容量、bootstrap、真实关联和零语义影响有独立门槛 |
| N29–N30 | GPU sidecar、Canonical/Derived/Audit Session、长录制 | A7 通过与后继 P3 差分失败同时保留 |
| N31 | Managed Catalog 开销与生命周期 | 计划和当前开关不等于最终全面性能通过 |
| QSCAN | 全量确定性统计、Profile、窗口、候选、状态和恢复 | 新规则不能沿用旧验收的完成标志 |
| Skill 2.1–2.3.1 | 源码深查、优先级分流、原生视图合同、帧序/分册 | 通用生成器和部分展示实现未全部跟上合同 |
| 9 月长数据与对账 | 磁盘索引、R20、Marker 层级、网页报告包 | 读取成功、语义一致、性能和报告解释分别验证 |
| 10 月当前路线 | EventStore、Stream analyzer、使用率/显存/FPS、网页 native 回查、2.6.3 | 包版本与 EXE 来源存在差异，mainline/dev 仍分开 |

详细旧资料见 [历史归档](history/README.zh-CN.md)。

## 关键设计选择及其原因

| 决策 | 原因 | 维护含义 |
|---|---|---|
| 全进程单 Client DLL | 避免多份静态 Client、重复队列和生命周期 | 引擎/Package/插件统一 ABI，构建排除重复实现 |
| 复用 Profiler 加专用事件 | 已有 CPU 标记丰富，但 Job/GPU/资源语义不能凭名称还原 | 通用转发与结构化专用接口并存 |
| Source Location 与动态栈分开 | 文件行号不代表运行时调用链 | 记录栈来源、覆盖、预算和实际捕获状态 |
| Query 拥有统计数值 | 避免模型/Python 重算口径与全量事件传输 | AI 做解释、假设和定向取证 |
| Stream 路由独立于旧 MCP | 原生批量分析和 EventStore 可直接读取大录制 | 新审计不冒充旧 Schema4/candidate-policy-v3 |
| 明确保留缺失和降级 | 源录制可能缺域或出现配对问题 | 不以空集合/零值掩盖查询失败或未记录 |
| 同实例视图身份 | 报告必须可人工回查 | Query ref、store ID、GUI 显示号分别处理 |
| 原 Player 完整副本 | 代码替换需要保留资源与插件并可回退 | build seed 不能当运行包，代码闭包必须同代 |

早期 SQLite 导出退出 AI 主流程与当前 EventStore 使用 SQLite 不冲突：前者是分析入口选择，后者是访问层实现。不要把旧决策概括成“项目禁止 SQLite”。

## 当前主要分支

| 分支 | 当前交接定位 |
|---|---|
| mainline | 本文指定的 Tracy 源码基线 f49190f2541c |
| dev | 含 mainline 及另外 4 个后续提交；不能当作本文同一版本 |
| feature/JNTracy-GpuCatalog-DiskIndex | R20 等长数据实现与验收历史入口 |
| feature/JNTracy-N30-LongTraceSessionStore | 独立 Session 路线，历史 8df7b0a1 状态 |
| feature/JNTracy-FixedSlices | 固定切片开发与阶段验收入口 |
| feature/JNTracy-AI-QueryScan / DevBase | 旧确定性扫描、提取与缓存演进 |
| feature/JNTracy-LocalReportViewer | 普通 Trace 网页回查历史 |
| feature/JNTracyUnity | 本次引擎基线 c2877d4b053c |
| feature/Pack-JNTracy | 本次 Package 基线 0ac6735ce3ca |

名称不能证明合并关系。完整祖先检查与 dev 独有提交见 [分支证据](evidence/branch-relations.json)，完整 HEAD 和对比基线见 [仓库快照](evidence/repository-snapshot.json)。

## 必须保留的结论纠正

- N20/N21 的“登录成功”因业务时序和 Init 场景假阳性作废，N22B 才是修正后的成功。
- Unity/Tracy 同名 Marker 未出现在同一路径，不代表事件迁移；SRP Batcher 三层链曾被重新核对并纠正。
- Query 替代分析可以形成有用报告，但不能宣称 tracy-analyze 原流程已通过；后来才有完整重跑。
- 4 GiB 失败、32 GiB 安装修订和 mainline 16 GiB 是不同身份。
- 低耗时窗口不自动是正常对照，格式通过不自动说明指标语义正确。
- 从旧报告升级到新报告仍可能丢失已取得的有效发现，需做正文保留和计时口径对账。

这些纠正应成为接手者的回归资产与阅读提示，而不是从正式交接中删掉的过程杂项。
