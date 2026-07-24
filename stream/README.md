# tracy-stream 流式采集模块

`tracy-stream` 是 Tracy 结构化数据的流式采集层。它提供可移植的
`.tracy-stream` 追加式日志，支持 CRC32C 记录校验、显式提交尾部、
持久化边界、恢复扫描和按需尾部修复。`tracy-capture` 通过严格的异步
双缓冲转发器持续写入日志。仅输出日志时，采集器使用有界的
`Worker::Mode::ProtocolOnly` 解析器，不保留完整时间线。
当前写入器会在 `SessionBegin` v2 中声明 ProtocolOnly 的延迟符号扩展
模式，因此尚未结束、还没有 v4 排空标记的实时 revision 也能按正确模式
严格重放。
`tracy-stream-convert` 可以严格重放有效日志前缀，并生成标准 `.tracy`
快照。

`JournalStore` 只为已经提交的前缀发布不可变读取视图。修订号
（revision）等于最后一条已提交记录的序号，水位时间戳（watermark）
等于最后一条已提交记录的单调时间戳。实时写入中的不完整尾部不会
改变这两个值。每个已发布前缀还带有 CRC32C 指纹，因此即使文件被
原地改写、截断或替换后仍能通过
结构校验，也不会覆盖上一份有效视图。

独立构建并测试：

```powershell
cmake -S stream -B stream/build -DBUILD_TESTING=ON
cmake --build stream/build --config Release
ctest --test-dir stream/build -C Release --output-on-failure
```

只检查日志，不修改文件：

```powershell
stream/build/Release/tracy-stream.exe inspect capture.tracy-stream --records 20
```

尾部修复必须显式执行：

```powershell
stream/build/Release/tracy-stream.exe recover capture.tracy-stream --truncate
```

同时记录流式日志和普通快照：

```powershell
capture/build/Release/tracy-capture.exe `
  -j output.tracy-stream -o output.tracy -s 30
```

两种输出都可以单独省略。仅指定 `-j` 时，记录器默认限制为 512 MiB
内存、8,000,000 个保留定义/状态项和 2,000,000 个排队定义查询。
超过任何限制，或发生写入、刷盘（flush）失败时，采集都会显式终止；
记录绝不会被静默丢弃。定时本地停止会记录 v4 `BeginDrain` 控制记录，
其中保存实时连接的服务器查询窗口。采集端随后发送 Tracy 原生
`ServerQueryDisconnect`：启用 `TRACY_ON_DEMAND` 的 Client 会停止接收
新的普通事件，同时继续发送断开前已经排队的事件和异步定义响应。
`ProtocolOnly` 仍解析调用栈的函数名、文件和行号，但不在结束阶段递归
扩展可选的符号机器码和反汇编地址。待停止时已有尾部和必要定义排空后，采集端
发送普通 `ServerQueryTerminate` 并写入 `SessionEnd`。

因此，定时停止或在游戏仍运行时主动结束一个完整的 ProtocolOnly journal
要求 Client 使用 `TRACY_ON_DEMAND`。非 on-demand Client 可以在被测进程
自然结束时完成录制，但不具备本轮已经验证的“主动断开后停止新事件并继续
响应定义查询”语义。

如果排空仍因异常 Client 或定义响应而无法结束，第二次按下 `Ctrl+C`
会以退出码 130 强制停止。此时不会伪造完整的 `SessionEnd`，但所有已经
提交的 journal 前缀仍可检查、恢复和转换。转换器和 Query 保持对旧版
v1、v2、v3 排空控制记录的读取兼容；当前新录制统一写入 v4。

无法添加 `-j` 参数的自动化程序，可以将当前进程的
`TRACY_STREAM_OUTPUT` 环境变量设置为日志路径。显式传入的 `-j` 参数
优先级更高。

转换完整日志或包含可恢复前缀的日志：

```powershell
capture/build/Release/tracy-stream-convert.exe `
  -i output.tracy-stream -o replayed.tracy
```

转换器通过 `Worker` 重放客户端字节，在保存快照前校验记录中的服务器
握手和查询字节流。ProtocolOnly journal 的 Full 重放保留完整时间线，
但使用录制器相同的排空完成条件。每条服务器记录还有 10 秒本地读取
截止时间；协议缺失或发散会显式失败，不会无限等待。

直接查询已提交的实时日志或完整日志：

```powershell
query/build/Release/tracy-query.exe `
  --trace output.tracy-stream --allow-root (Split-Path output.tracy-stream)
```

`SegmentTraceSource` 会严格重放每个不可变的已提交修订版本，再以原子
方式发布。已经使用旧修订版本的请求会继续持有原视图；部分写入或
无效刷新无法替换上一份有效修订版本。

运行无人值守的生产者、采集、重放和 MCP A/B 验收流程：

```powershell
stream/tests/ProtocolCaptureAcceptance.ps1 `
  -ProducerExe stream/build/Release/tracy-stream-test-producer.exe `
  -CaptureExe capture/build/Release/tracy-capture.exe `
  -ConverterExe capture/build/Release/tracy-stream-convert.exe `
  -InspectorExe stream/build/Release/tracy-stream.exe `
  -QueryExe path/to/tracy-query.exe `
  -OutputRoot stream/build
```

更多信息请参阅[格式约定](docs/format-v1.md)和
[分阶段实施计划](docs/implementation-plan.md)。
[风险与后续改进备忘录](docs/risks-and-future-improvements.md)记录了
真实超长排空事故、已经实施的定向修复，以及仍暂不实施的背压指标、
Client 队列硬上限、完整离线符号化、自动转换和 GUI 支持方案。
[Unreal Insights 源码分析](docs/unreal-insights-streaming-analysis.md)
记录了本设计采用的背压、实时读取和可靠性行为。
[验收结果](docs/acceptance-results.md)包含真实 Godot/TPS
`ProtocolOnly` 运行结果，[仓库状态报告](docs/repository-state.md)记录了
最终的单工作树（worktree）布局。
