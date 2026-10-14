# Tracy 仓库源码地图与改动入口

本页按 mainline 的模块关系组织阅读入口。完整变更范围在 [Tracy 文件清单](appendices/tracy-file-changes.zh-CN.md)，每个具体入口的 SHA 与行号在 [来源索引](appendices/source-index.zh-CN.md)。

## 目录职责

| 目录 | 职责 | 优先入口 |
|---|---|---|
| public/client 与 public/common | Client 协议、队列与 JN 扩展共享定义 | [TracyProtocol.hpp:12](../public/common/TracyProtocol.hpp#L12) |
| server | Worker 解析、持久化与普通 Trace 数据模型 | 与 query/analysis 的 TraceSource 实现配合阅读 |
| capture | 后台采集和旧离线转换入口 | [capture.cpp:1](../capture/src/capture.cpp#L1) |
| capture-gui | Player 启动、配置核对、进程编排、录制/建库状态 | [Controller.cpp:244](../capture-gui/src/Controller.cpp#L244)、[Process.cpp:1](../capture-gui/src/Process.cpp#L1) |
| stream | Journal、EventStore、导出、索引、校验 | [TracyStreamJournal.cpp:1](../stream/src/TracyStreamJournal.cpp#L1)、[TracyEventStore.hpp:14](../stream/src/TracyEventStore.hpp#L14)、[TracyEventStoreExport.inl:1](../stream/src/TracyEventStoreExport.inl#L1) |
| storage | 磁盘存储与预算相关基础设施 | 与具体 store/index 消费者一起阅读 |
| analysis | 与存储解耦的 DTO、扫描、统计、分类、缓存与内存保护 | [TracyFrameTriage.cpp:1](../analysis/TracyFrameTriage.cpp#L1)、[TracyFrameTriage.hpp:1](../analysis/TracyFrameTriage.hpp#L1) |
| query | CLI、MCP、Session、公共方法路由、原生分析器 | [TracySessionManager.cpp:81](../query/src/TracySessionManager.cpp#L81)、[TracyQueryService.cpp:1](../query/src/TracyQueryService.cpp#L1)、[TracyMcpServer.cpp:1](../query/src/TracyMcpServer.cpp#L1)、[analyze.cpp:249](../query/src/analyze.cpp#L249) |
| profiler | 普通时间线、流式时间线、资源界面和报告导航 | [TracyStreamingView.cpp:1](../profiler/src/profiler/TracyStreamingView.cpp#L1)、[TracyStreamingTimeline.cpp:1](../profiler/src/profiler/TracyStreamingTimeline.cpp#L1)、[TracyView_ReportNavigation.cpp:1](../profiler/src/profiler/TracyView_ReportNavigation.cpp#L1) |
| skills | 旧版与 V2 Skill、Python/PowerShell 编排、模板、校验器、工具包 | [SKILL.md:1](../skills/diagnose-jn-unity-tracy/SKILL.md#L1)、[SKILL.md:1](../skills/diagnose-jn-unity-tracyV2/SKILL.md#L1) |
| doc | 当前维护手册、变更清单与历史归档 | [文档首页](README.zh-CN.md) |

## 录制入口如何追

从 `Controller::Work` 开始：检查工具和 PID/INI → 建立任务目录 → 启动 Query 预检 → capture 写 journal → exporter 跟随建库 → stop-file 和排空 → 校验源与 store summary → 发布 task。恢复流程还会处理已有进程、旧 converter 以及原 stream 改变后的失效产物。

控制器当前要求的后台程序为 capture、stream-export、query。旧 README 中必须配套 converter 并产出 .tracy 的描述已经落后于代码。[Controller.cpp:244](../capture-gui/src/Controller.cpp#L244)

## 数据文件如何进入 Query

`SessionManager::Open` 解析允许根目录与路径、创建异步 session；loader 使用 TraceSource。读取路径不是由文件大小猜测，而是格式、可用 store 和 preferIndex 决定。至少核对 Queued/Loading/Ready/Failed、source kind、revision、watermark、complete 与错误码。

EventStoreTraceSource 的完整查询仍可准备小域 Worker skeleton；大量 CPU/GPU/Job/Gfx 数据从 store 读取。不能把“存在一个小 skeleton 文件”误判为已经全量转出了 .tracy，也不能把 skeleton 当完整独立原始录制。[TracySessionManager.cpp:81](../query/src/TracySessionManager.cpp#L81) [TracyEventStoreTraceSource.cpp:67](../query/src/TracyEventStoreTraceSource.cpp#L67)

## 两套分析实现入口

| 路线 | 入口 | 维护重点 |
|---|---|---|
| 旧确定性 Scan | AnalysisScanManager，经 MCP tracy_scan 路由 | Profile、Neutral/Policy identity、持久状态、候选分页与恢复 |
| Stream 原生分析 | query/src/analyze.cpp 与 FrameTriage | 完整帧与角色分区、CPU/GPU独立信号、VRAM候选、缓存与追加窗口 |

新 CLI 不读取旧 YAML module budgets；旧 Scan 也不能因新分析器成功就自动视为相同算法。[TracyAnalysisScanManager.cpp:1](../query/src/TracyAnalysisScanManager.cpp#L1) [analyze.cpp:249](../query/src/analyze.cpp#L249) [TracyFrameTriage.cpp:1](../analysis/TracyFrameTriage.cpp#L1)

## 网页回查的真实代码位置

Stream 网页工具当前位于 **skills/diagnose-jn-unity-tracyV2/tools/stream-report-viewer**。某些引用仍写 profiler/stream-report-viewer，但该目录在此 mainline 不存在。原生部分在 StreamingView/StreamingTimeline，Python 部分负责注册表、服务和导图；普通 Trace 的报告导航仍有独立 C++ 文件。[server.py:1](../skills/diagnose-jn-unity-tracyV2/tools/stream-report-viewer/server.py#L1) [prepare.py:1](../skills/diagnose-jn-unity-tracyV2/tools/stream-report-viewer/prepare.py#L1) [TracyStreamingView.cpp:1](../profiler/src/profiler/TracyStreamingView.cpp#L1)

## 改动影响范围

| 改动对象 | 需要同时检查 |
|---|---|
| 协议或 JN 事件布局 | Unity 内嵌 Client、Tracy Reader/Writer、Store、Query、Profiler、fixture |
| EventStore schema | builder、manifest、直接 reader、skeleton、GUI、analyze 与旧库重建行为 |
| CPU/GPU ref 或帧号 | tree 查询、报告 view spec、原生导航、导图回执和已有报告 |
| 分类算法或阈值 | policy identity、缓存失效、候选数量/严重帧覆盖、对比报告 |
| Query 字段 | method registry、MCP 参数、schema、字段可用性与消费者 |
| Skill 命令或字段 | wrapper、配置示例、审计器、报告组装、安装器与说明 |

开发时先做最小复现和对应数据域测试，再做真实样本验证。只改显示文案可以做文档/展示检查；改变事件、统计或身份时，必须用实际数据和负向用例证明没有静默遗漏。
