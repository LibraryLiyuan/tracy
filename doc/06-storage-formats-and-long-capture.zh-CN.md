# 存储格式 长录制与数据访问

不同存储路线保留了不同层次的数据和读取成本。接手者应先识别原始事实、派生产物和消费者，而不是只按扩展名或目录体积判断用途。

## 介质与职责

| 介质 | 保存什么 | 当前用途与限制 |
|---|---|---|
| .tracy-stream | 双向协议记录、提交边界和原始录制身份 | 原始 journal；中间字节不保证能独立重放 |
| .tracy-stream.store | 当前 schema17 的发布 EventStore | Stream Query、原生分析和流式 GUI 的主要读取层 |
| 小域 skeleton | 完整查询所需的部分元数据和小域 | 与 store 配合；不是可替代完整录制的独立快照 |
| .tracy | Worker 可读取的持久化快照 | 普通 Trace 路线；需要匹配标准/扩展 reader |
| .jnidx 与分析缓存 | 加载索引、中立统计、候选和恢复状态 | 不等于原始数据；身份和算法变化会失效 |
| .jn-trace-session | N30 的 Canonical/Derived/Audit 容器 | 独立历史分支，非当前 EventStore 的别名 |
| FixedSlices | 固定核心时间窗及必要上下文的独立片 | 独立开发分支，总体验收未完成 |

## Journal 的提交边界

JournalStore 为已提交前缀发布 revision/watermark；正在写入的不完整尾部不能被当成已提交事实。CRC、前缀指纹、源 SHA 和 clean-end 描述的是不同检查层次。正常停止要保留异步定义响应和已有事件尾部；强制截断后的可恢复前缀不等于原录制完整。[TracyStreamJournal.cpp:1](../stream/src/TracyStreamJournal.cpp#L1)

分割 journal 不能只按时间点或文件字节剪开：压缩状态、字符串、SourceLocation、调用栈定义和跨界生命周期可能位于前文。旧切片方案正是为这些依赖建立显式边界合同，而不是普通文件 split。

## EventStore 当前结构

EventStore 用 SQLite 和压缩记录组织可分页事实，schema 为 17。Open 可进行内容校验；快速路径可先接受登记的大小、时间和文件身份，再另行 VerifyContent。schema 不匹配的库需要按实现重建，不应修改 manifest 中的版本数字冒充兼容。[TracyEventStore.hpp:14](../stream/src/TracyEventStore.hpp#L14) [TracyEventStore.cpp:1](../stream/src/TracyEventStore.cpp#L1)

StoreOptions 默认 cache 64 MiB、result 16 MiB、disk 64 GiB。它们是某一组件的预算，不是整个 Query/Profiler/analyze 进程的峰值上限。独立 reader、结果缓冲、Worker skeleton 和导出临时表还会占用资源。

当前 GUI 录制期间启动 exporter follow，停止后等待库完成；若没有合法 summary，使用最终 journal 再构建。summary 必须绑定源 SHA，存在不同源内容的旧 store 时会另行保留失败产物，不能无声覆盖后当作同一录制。[Controller.cpp:244](../capture-gui/src/Controller.cpp#L244)

## 为什么直接读 stream 仍可能出现 skeleton

EventStoreTraceSource 对部分小域复用 Worker 元数据结构，大量 CPU/GPU 时间线、Job、Gfx 和关系从 store 读取。skeleton 导出明确省略这些大域的完整事件，因此“没有全量转换”与“完全没有任何 .tracy 形态的临时文件”不是同一承诺。[TracyEventStoreTraceSource.cpp:67](../query/src/TracyEventStoreTraceSource.cpp#L67) [TracyEventStore.hpp:14](../stream/src/TracyEventStore.hpp#L14)

维护时需检查 skeleton 与 journal/store 的强身份、schema、source revision、读取预算和缓存失效；不要把旧 skeleton 直接复用于变更后的录制。

## Query 路由与 analyze 路由

- 通用 Query：当前 store 可用时走 EventStoreTraceSource；否则 stream 可以走 SegmentTraceSource。普通 .tracy 可以走 Worker 或 QueryIndex。
- tracy-analyze：当前要求已发布、当前 schema 的 EventStore；缺少 store 时拒绝，不自动完整转换。
- Streaming Profiler：使用自己的流式视图和分页时间线；能打开 Query 不自动证明 GUI 的全部面板或规模都可用。

[TracySessionManager.cpp:81](../query/src/TracySessionManager.cpp#L81) [TracySegmentTraceSource.cpp:1](../query/src/TracySegmentTraceSource.cpp#L1) [analyze.cpp:249](../query/src/analyze.cpp#L249) [TracyStreamingView.cpp:1](../profiler/src/profiler/TracyStreamingView.cpp#L1)

## 普通 Trace 和历史离线优化

旧离线路线进行了 CPU/GPU Zone、字符串、调用栈、采样、Pass/Catalog 等磁盘化工作，并形成 JNTRACYX 扩展格式。旧格式和标准 Trace 曾出现 reader 选错导致“文件打不开”的案例，所以产物必须连同正确工具版本交付。

R20 的 Query 长录制验收使用 12.43 GB 已发布 Trace/schema49 索引，Scan 52 分 04.117 秒，总流程 74 分 12.541 秒；它不是新 EventStore 的性能基准，也未重测冷建恢复。[Report.zh-CN.md:1](C:/Users/Admin/Documents/JN-Unity-T3/GpuCatalog-DiskIndex/Query-Final-Acceptance-20260929-r20/Report.zh-CN.md:1)

## N30 与固定切片的接手位置

N30 将 Canonical 事实、Derived 索引和 Audit 纳入 Session generation。历史 A7 曾通过指定长录制，后续 P3 又发现统计单位、Job/帧数量和慢查询差异。两类结果需同时保留，不能用前一次成功覆盖后一次失败。

FixedSlices 分支要保证每片携带必要定义与边界语义、保留原始身份、不把切掉的子节点造成的 self 变化伪装成原始 self。最新记录包含短流 CPU/Frame 投影阶段通过和后续修复，原大源、全部域及 GUI 消费者仍待完成。[README.md:1](C:/Users/Admin/Documents/JN-Unity-T3/Tracy-Fixed-Slice-Evidence/20261007-mcp-parity-001/README.md:1)

## 产物清理与复用

原始 journal、冻结结果、manifest、EXE 身份和必要符号属于复现证据。索引或缓存只有确认可重建、没有读者使用并与既有报告关系清楚后，才适合清理。不要按文件名带 building/temporary 就直接判断无用；历史运行需要读取 CURRENT、发布状态、租约与失败记录。

跨机器分享要区分：仅阅读报告、原生回查、完整重新分析。后两者分别需要匹配工具及完整的录制/store/身份信息，不能只发单个 Markdown 或目录内某一 shard。
