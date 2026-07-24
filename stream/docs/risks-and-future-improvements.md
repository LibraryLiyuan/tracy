# Tracy 流式采集风险与后续改进备忘录

## 1. 当前决策

记录日期：2026-07-24。

当前 `.tracy-stream` 功能已经完成格式、写盘、恢复、严格重放、
`ProtocolOnly`、直接 Query/MCP 访问以及真实 Godot/TPS 验收。本轮讨论
识别出的风险目前不作为阻塞项，也不立即修改实现。继续保留当前经过验收
的严格行为，等实际录制、性能基准或长时间运行暴露问题后，再依据本文件
中的触发条件选择改进项。

这份文件是风险登记和设计备忘录，不表示下列改进已经实现。

## 2. 当前架构和工具边界

Tracy 所说的 Client 是嵌入被测程序中的库，而不是 `capture.exe`。
当前进程关系如下：

```text
game.exe
└─ Tracy Client 库
   ├─ 游戏线程执行 Tracy 宏：取时间戳并把事件放入队列
   ├─ Tracy Profiler 后台线程：消费、压缩并通过 TCP 发送
   ├─ Tracy Symbol Worker：处理调用栈和符号查询
   └─ 可选图片压缩、系统追踪和采样线程
                    │
                    │ TCP，通常只允许一个实时接收端
                    ▼
        ┌──────────────────────────────┐
        │ tracy-profiler.exe（GUI）    │
        │ 或 capture.exe（无界面）     │
        └──────────────────────────────┘
```

- GUI 的 Connect 和 `capture.exe` 是两个可替换的实时接收端，不是串联
  的两个采集步骤。
- `capture.exe` 是独立进程；`.tracy-stream` 写盘发生在该进程的 journal
  writer 线程。
- 事件产生、按事件抓取调用栈、动态定义读取和符号查询响应仍发生在
  `game.exe` 内。
- `tracy-query.exe` 和 `tracy-stream-convert.exe` 是离线读取工具，不与
  正在运行的 Tracy Client 建立采集连接。

当前输出行为如下：

| 调用方式 | 实时 Worker 模式 | 输出 | 说明 |
| --- | --- | --- | --- |
| 只指定 `-j` | `ProtocolOnly` | `.tracy-stream` | 不保留完整时间线，录制内存有硬限制 |
| 只指定 `-o` | `Full` | `.tracy` | 传统 capture 行为 |
| 同时指定 `-j` 和 `-o` | `Full` | 两种文件 | 实时保留完整时间线，失去 ProtocolOnly 的低内存优势 |
| `tracy-stream-convert` | 离线 Full 重放 | `.tracy` | 严格校验记录过的服务器查询字节 |

`.tracy-stream` 当前不能直接由 Tracy GUI 打开。`tracy-query.exe` 和 MCP
可以直接接收该扩展名，但内部实现会将一个不可变的已提交 revision
严格重放为临时 `.tracy`，再复用现有查询数据源。它具备正确的已提交
前缀语义，但还不是原生增量索引。

## 3. 已识别风险

### 3.1 慢磁盘和背压会传递到游戏进程

当前采集端不丢协议数据。写盘持续慢于数据产生速度时，压力链为：

```text
SSD 写入/Flush 变慢
→ journal writer 来不及消费
→ 协议观察器的等待槽被占满
→ capture 的网络处理线程停止继续读取
→ TCP 接收窗口逐渐填满
→ game.exe 内 Tracy Profiler 线程的发送变慢或阻塞
→ Tracy Client 队列不能及时消费
→ 游戏进程内事件队列增长
```

普通事件在产生时已经取得时间戳，因此单纯的网络延迟不会把事件时间戳
向后移动。但是队列分配、内存增长、缓存和内存带宽、分页、共享 CPU，
以及少数串行队列锁竞争，都可能间接改变被测程序。

当前 `ProtocolJournalOptions::bufferBytes = 512 KiB` 主要是单条协议记录
的最大值。转发器只有一条正在写的记录和一条等待记录。Tracy 压缩帧的
原始目标上限是 256 KiB，因此 512 KiB 足够保证协议记录合法；单纯把
这个数字改成 8 或 16 MiB，不会让“一条等待记录”自动变成多条队列，
也不会显著增加普通工作负载下的抗突发时间。

采集端有界不等于端到端有界。Tracy Client 的普通事件队列仍会按需
分配新块，因此长时间背压可能把内存压力转移到 `game.exe`。

### 3.2 特殊串行事件存在锁竞争和整块扩容风险

普通 CPU Zone 主要使用每线程生产者的并发队列。要求全局顺序的一部分
事件使用 `m_serialLock + m_serialQueue`，包括：

- 内存分配和释放；
- Tracy 锁事件；
- 部分 D3D11、D3D12、Vulkan、CUDA、OpenCL 等 GPU 事件；
- 串行调用栈以及若干必须保持跨线程顺序的事件。

当前 `m_serialQueue` 和 `m_serialDequeue` 各预分配 1,048,576 个
`QueueItem`。每项为 32 字节，两份初始存储合计约 64 MiB。该初始容量
已经很大，不宜只通过继续扩大初始容量处理风险。

如果活动串行队列超过容量，`FastVector::AllocMore()` 会分配两倍容量，
复制全部旧元素，再释放旧内存。生产者调用 `prepare_next()` 前已经持有
`m_serialLock`，所以触发扩容的游戏线程会在锁内承担大块分配和复制；
其他同时记录串行事件的线程也会等待。只有队列真的积压到一百万项以上
才会触发这一情形，但一旦触发，可能产生明显长尾。

采集端 512 KiB 设置与这个 Client 串行队列不是同一层缓冲，扩大前者
不能直接解决后者的锁和扩容问题。

### 3.3 Tracy Client 与游戏共享进程故障域

Tracy Client 嵌入 `game.exe`，但发送、符号解析和图片压缩通常使用独立
后台线程。如果只有游戏主线程死循环、等待锁或长时间阻塞，而进程仍可
调度：

- Tracy Profiler 线程通常仍能发送已经入队的事件；
- 卡住前的 `ZoneBegin` 可以存在，但不会再产生对应 `ZoneEnd`；
- 后续 Frame Mark 会停止；
- 启用系统采样或上下文切换追踪时，可能看到热点指令或等待状态。

以下情况仍可能丢失尚未发出的尾部：

- 整个进程被暂停、崩溃或强制终止；
- 内存破坏影响 Tracy 后台线程；
- 系统级故障使进程线程无法调度；
- 数据仍在 Client 队列里，尚未通过 TCP 发出。

独立 `capture.exe` 能保住已经收到并提交的 journal 前缀，但不能恢复只
存在于已终止游戏进程内存中的队列数据。必须诊断硬卡死时，可考虑以后
组合 ETW/WPR、ProcDump 或外部 watchdog 抓取线程栈。

### 3.4 采集进程和游戏仍共享机器资源

当前使用 SSD，顺序写盘带宽风险显著低于机械硬盘，但不等于没有干扰：

- 游戏资源读取、日志、shader cache、页文件和 journal 可能共享同一
  存储队列；
- 每秒或每 16 MiB 的持久化检查点可能触发短时 Flush 延迟；
- `capture.exe`、游戏和符号线程共享 CPU、内存带宽和缓存；
- SSD 固件垃圾回收、系统安全软件和其他 I/O 也可能制造偶发长尾。

目前测试证明数据完整性和有界采集端行为，没有进行“关闭 Tracy”与
“开启流式录制”的受控主线程 p99/p999 A/B，因此不能把当前验收解释为
零性能扰动。

### 3.5 定义查询排空可能显著延长结束时间

Tracy 0.13.1 协议是双向的。事件经常只带字符串指针、SourceLocation
ID、线程 ID、Callstack ID、指令地址或符号地址。服务器发现未知引用后
会向游戏内 Client 查询定义。

Tracy 原有的完整性原则是：Client 自然结束并发送 Terminate 后，Worker
等待 pending strings、threads、source locations、callstack frames、
symbols 等处理完成，再结束完整 trace。这个“引用需要定义”的原则属于
Tracy 原始设计。

当前 ProtocolOnly 新增的部分是本地 `BeginDrain` 边界：

1. 等待停止时已发起的网络读取完成；
2. 在 journal 中记录不属于 TCP 重放字节的本地边界；
3. 边界后的普通时间线数据仍原样写入 journal，但转换时不纳入时间线；
4. 继续发送和接收边界前必要的定义查询、响应和 ACK；
5. 定义响应产生新的子帧或符号查询时继续处理，直到依赖闭包完成；
6. pending、queued 和 in-flight 全部归零后发送普通
   `ServerQueryTerminate`，再写 `SessionEnd`。

因此，定义积压很多时，停止采集不等于进程立即结束。长排空期间游戏内
Tracy Symbol Worker 等线程仍可能工作。

真实 Godot/TPS 高细节验收数据：

- 有效采集约 3.06 秒；
- 响应式定义排空约 176.5 秒；
- journal 为 189,311,601 字节；
- 37,730,444 个协议事件；
- 3,619,879 个保留定义/状态；
- 321,857 个调用栈 payload；
- 2,925,163 个调用栈帧记录；
- 226,392 个符号。

在当前 SSD 环境中，189 MB 文件本身不足以解释 176.5 秒结束长尾，主要
嫌疑是调用栈、符号和定义查询链。不过当前尚无逐次写入/Flush 延迟指标，
不能仅凭总文件大小完全排除偶发 I/O 停顿。

### 3.6 高风险使用场景

#### 数小时高频全量调用栈

例如 120 FPS、几十个工作线程，大量 Zone、内存分配、锁和任务都抓取
16 到 62 层调用栈。调用栈展开本身发生在事件生产线程；数据产生速度
持续超过发送和定义解析速度后，Client 队列、journal 大小以及结束排空
时间都可能不断增长。

#### 对 p99/p999 极敏感的基准

p99 关注最慢 1%，p999 关注最慢 0.1%。平均帧时间可能几乎不变，但一次
队列扩容、内存页提交、串行锁竞争、Flush 或符号线程争用就可能污染尾部
样本。如果目标是判断几十微秒到几百微秒的回归，必须进行同配置 A/B。

#### 不允许 Client 队列增长的生产环境

例如在线服务运行数天，只有固定内存预算，而采集端经历分钟级网络或 I/O
异常。严格无损会让游戏进程队列增长，可能引发分页、OOM 或服务不稳定。

在下游长期慢于上游时，以下三点不能同时保证：

1. 绝不丢数据；
2. 内存严格有界；
3. 永不阻塞或影响生产者。

当前设计选择“无静默丢失 + 采集端有界”，所以必要时会产生 TCP 背压；
Client 端是否也需要硬界限和主动终止策略，应由真实问题决定。

## 4. 暂缓实施的改进方案

### 4.1 录制结束后自动生成 `.tracy`

建议未来增加：

```powershell
tracy-capture.exe `
  -j output.tracy-stream `
  --finalize-to output.tracy
```

期望行为：

1. 实时阶段始终使用 `ProtocolOnly`；
2. journal 完成 `SessionEnd` 和持久化后才开始转换；
3. 严格重放到 `output.tracy.partial`；
4. 验证和写入全部成功后原子改名为 `output.tracy`；
5. 默认保留 `.tracy-stream`，失败时可重试；
6. journal 不完整或 resolver 失败时不伪装成正常完整快照。

不应简单复用当前 `-j + -o`，因为该组合会在实时阶段使用 Full Worker。
建议把 converter 和 Query 中重复的严格重放代码提取为共享
`TracyStreamReplay` 库，供 converter、capture 自动完成、Query 和未来
GUI 共用。

自动转换发生在游戏断开之后，不会继续请求游戏数据；但它依赖 journal
中已经记录的定义响应，所以本身不能消除当前严格排空。

### 4.2 真正扩大采集端突发缓冲并补充指标

如果后续观察到短时 SSD/Flush 背压，建议把配置拆为：

```text
maxRecordBytes   = 512 KiB
queueBudgetBytes = 16 MiB（可配置）
```

队列允许多条记录，只要总实际 payload 不超过预算。应同步增加：

- 背压等待次数、累计时间和最长单次时间；
- 当前/峰值队列字节和记录数；
- 写盘吞吐、单次写入延迟和 Flush 延迟；
- Client 可观测队列深度或内存压力；
- 可选 trace marker，标记发生背压的时间范围。

扩大缓冲只能吸收短时抖动。持续慢盘仍应背压、主动停止或采用可配置
降级策略。

### 4.3 减轻 Client 串行队列风险

优先措施：

1. 降低串行高频事件和每事件调用栈数量；
2. 添加 Client 队列高水位指标；
3. 达到高水位时关闭完整类别或结束采集，而不是单独丢失 Alloc/Free；
4. 若实测达到 FastVector 容量，再将其改为固定大小 chunk 的分块队列，
   避免扩容时复制全部历史元素；
5. 只有基准证明全局锁本身是瓶颈时，才评估更激进的 MPSC/序列合并
   设计。

不建议仅扩大当前一百万项初始容量，也不建议在没有严格顺序测试的情况
下改写 Tracy 的跨线程串行语义。

可考虑的压力策略：

```text
50%：记录告警
75%：停止请求可选源码、符号代码或深调用栈
90%：请求本次采集正常结束
100%：保留有效前缀并断开，避免游戏 OOM
```

具体阈值必须通过真实数据确定，当前未实现。

### 4.4 拆分 Godot 调用栈采集策略

当前 Godot `profiler_tracy_full_capture=yes` 要求同时启用
`profiler_sample_callstack=yes` 和 `profiler_track_memory=yes`；
`profiler_sample_callstack=yes` 会设置 `TRACY_CALLSTACK=62`。这使普通
Zone、消息和内存宏共享很深的默认调用栈策略。

建议未来拆分为：

```text
profiler_system_sampling=yes/no
profiler_zone_callstack_depth=0/8/16/62
profiler_memory_callstack_depth=0/8/16/62
profiler_message_callstack_depth=0/8/16/62
```

可提供三种预设：

- 短时最高精度：深调用栈和完整内存；
- 一般流式采集：浅调用栈，保留系统采样和关键事件；
- 长时间采集：系统采样为主，只对怀疑模块启用按事件调用栈。

这会改变采集信息量，只有实际性能或排空问题出现后再实施。

### 4.5 将昂贵符号解析移到离线阶段

游戏结束后可以离线处理的内容主要包括：

- 原始指令地址到函数名、文件、行号；
- inline frame；
- PDB/DWARF；
- 源码和符号机器码读取。

前提是录制期间已经保存：

- 原始 Callstack ID 到指令地址列表；
- 模块基址、大小和路径；
- PDB GUID/Age 或等价 Build ID；
- ASLR 转换信息；
- 与采集匹配的 EXE、DLL、PDB 或符号服务器信息。

不能可靠地只靠离线文件恢复：

- 运行时动态字符串和动态 Zone 名；
- Godot Script/JIT 动态 SourceLocation；
- 线程和 Fiber 运行时名称；
- Frame Image；
- 只存在于进程内存中的其他动态定义。

因此建议的是混合 `DeferredSymbols` 模式，而不是完全取消在线定义：

```text
在线短排空：
  动态字符串、线程/Fiber 名称、SourceLocation 原始值、
  Callstack ID → 原始地址、模块映射、Frame Image 等不可替代数据

离线完成：
  地址 → 函数/文件/行号、inline frame、PDB/DWARF、源码和机器码
```

应保留两种模式：

```text
--definition-mode strict
--definition-mode deferred-symbols
```

`strict` 继续作为兼容性和正确性基准。`deferred-symbols` 需要协议/格式
扩展、外部符号根配置，以及与严格模式的 Zone、地址、模块、符号和 GUI
结果 A/B，不应直接修改现有 Full Worker 默认行为。

### 4.6 GUI 支持 `.tracy-stream`

可以分三阶段评估：

1. **固定 revision 打开**：严格重放为临时 `.tracy`，再使用现有 GUI；
2. **手动刷新**：显示 revision、watermark 和 complete，用户主动刷新到
   最新有效 revision；
3. **原生实时增量**：GUI 直接消费增长的 segment，不反复生成完整快照。

第一阶段工作量中等。第三阶段改动较大，因为当前 GUI `View` 直接持有
`Worker` 并缓存时间线指针、选择状态和统计对象；频繁替换 Worker 会使
指针和 UI 状态失效。只有实际需要 GUI 浏览未结束的流文件时再开始该
里程碑。

### 4.7 生产环境的可配置策略

如果未来用于长期生产采集，应明确选择：

- 严格无损：允许背压和较长结束等待；
- 有界影响：达到压力阈值后结束采集并保留有效前缀；
- 自适应降级：按类别停止可选调用栈、源码、符号代码或高频事件。

内存事件、锁事件等存在配对和顺序关系，不能在不了解语义的情况下随机
丢单条事件。

## 5. 重新评估触发条件

出现以下任一现象时，应回到本文件选择对应改进，而不是预先改动：

- `BackpressureWaitCount` 持续增长，或累计/最长等待达到可观察量级；
- 开启流式录制后游戏主线程帧时间、p99 或 p999 明显回归；
- `game.exe` 内存随采集时间或慢盘持续增长；
- 串行队列达到扩容点，或游戏线程在 `m_serialLock` 上出现可见等待；
- journal 写入吞吐低于输入速度，或 Flush 出现明显长尾；
- 排空时间长期超过有效采集时间，影响自动化或游戏退出；
- 符号线程在排空期间造成可测 CPU/内存争用；
- 自动化明确需要结束后立即获得标准 `.tracy`；
- 用户需要 GUI 浏览尚未结束、持续增长的 `.tracy-stream`；
- 严格转换出现未解析定义、协议发散或频繁不完整结果；
- 生产环境要求 Client 内存硬上限或有界性能影响。

重新评估时应先采集指标和复现，再决定是调整生产者信息量、增加短时
缓冲、优化队列、设置主动终止策略，还是实施离线符号化。

## 6. 当前保留结论

- 保持当前严格、无静默丢失的 `.tracy-stream` 行为；
- 保持 `BeginDrain` 和严格字节重放作为正确性基准；
- 不立即扩大 Client 串行队列或改变 Tracy 原始协议语义；
- 不立即实施自动转换、DeferredSymbols 或 GUI 原生增量；
- 当前录制可继续使用，实际出现性能、资源或结束延迟问题后按触发条件
  定向改进。

相关现状和实测数据见：

- [实现与验收计划](implementation-plan.md)
- [验收结果](acceptance-results.md)
- [Unreal Insights 流式实现分析](unreal-insights-streaming-analysis.md)
- [`.tracy-stream` 格式约定](format-v1.md)
