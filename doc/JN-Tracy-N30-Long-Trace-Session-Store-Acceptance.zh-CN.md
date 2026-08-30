# JN Tracy N30 长时间 Trace Session Store 验收记录

## 1. N30.0 基线

记录时间：2026-08-30。

### Git

```text
Repository: C:\CodeProjects\GodotProjects\tracy-0.13.1-n30-long-trace-session
Branch:     feature/JNTracy-N30-LongTraceSessionStore
Base HEAD:  15ceef123bed1ae0a87ed0cb4af908c3b19e83e8
Subject:    Implement N29 GPU resource analysis sidecar
```

N30 从 N29 独立 worktree 开始；不得修改或合并 `dev`、N29 worktree 和其他现有 worktree。

### N29 工具基线

| 工具 | 大小 | SHA-256 |
|---|---:|---|
| `build-n29-capture\Release\tracy-stream-convert.exe` | 11,300,352 | `7930C68F01B5CDFAB2C7990A4A9EC08FC9108B34076BF8A7A48AD6C4E5407214` |
| `build-n29-query\Release\tracy-query.exe` | 15,267,840 | `D40A151ADA7FFA8CCCCBB286E5F94875BACF1BF9DED43FC7BA7E1143C99A37BC` |
| `build-n29-query\Release\tracy-gpu-analysis-build.exe` | 11,512,832 | `1F38C058F7FB9BEC956960AD0B764B6980016279883386FCB356D1442CB1F76B` |
| `build-n29-profiler\Release\tracy-profiler.exe` | 27,313,152 | `5A6CA7B43B6A041B88C7183D2D37ADB54F2FC05BCF5320728074E13C2A35E149` |

版本基线：Protocol 90、JN section 12、Query API 1.33、N29 GPU Analysis schema 1。

### 短 Trace Oracle

```text
C:\Users\Admin\Documents\JN-Unity-T3\N29-Autonomous-Smoke\Smoke-20260830-214952
```

已知结果：

- 90秒自动Player流程成功。
- Stream：1,235,094,582 bytes。
- Trace：616,456,038 bytes。
- N29 GPU sidecar：4,530,300,324 bytes。
- GPU Resource records：1,498,592。
- GPU Allocation records：495,993。
- GPU Passes：1,969,665。
- GPU Uses：26,533,453。
- EngineKnownPhysical peak：4,140,236,800 bytes。
- FrameImage：75张，960×540，间隔60帧。

该数据用于传统Full Worker、N29 standalone sidecar和N30 Session之间的差分Oracle。

### 真实30分钟压力输入

```text
Path:
C:\Users\Admin\Documents\JN-Unity-T3\N29-Autonomous-30m\Run-20260830-220012\Admin-HighEvidence-30m.tracy-stream

Size:
19,122,822,908 bytes

SHA-256:
2AC46C53257CE9027EC67E098FC15070FB911243F6CD311A166EB97547848068
```

N29转换基线：

```text
DecodeBuild progress: 42.6%
Process commit:        40.83 GiB
Failure:               system memory pressure exceeded the offline conversion safety limit
Projected full replay: approximately 90–100 GiB
```

该失败发生在N29 GPU Analysis Builder开始之前，证明N29解决GPU分析读取问题，但未解决完整Worker replay的总量内存问题。

## 2. 阶段状态

| 阶段 | 状态 | 结果 |
|---|---|---|
| N30.0 隔离、计划与Oracle | Passed | 独立分支/worktree、正式计划、Oracle和30分钟失败基线已提交。 |
| N30.1 Schema与原子存储 | Passed | Schema 1、强身份、Shard校验、多generation原子发布和查询门禁通过。 |
| N30.2 Inventory与容量预检 | Passed | Journal、强身份、容量、Protocol QueueType、数据域、CaptureEnd质量及immutable依赖run通过真实30分钟输入。 |
| N30.3 Canonical/Checkpoint | Passed | 按17域对齐分片、共享Reader、ThreadContext/raw TSC、record-boundary安全取消、LZ4 checkpoint、单writer lease、强身份/损坏拒绝及内存/磁盘门禁已通过synthetic。生命周期索引从Canonical重建，不进入转换恢复checkpoint，避免重复维护第二套权威状态机。 |
| N30.4 全Canonical域 | Passed | Protocol 90全部QueueType均按17域保存原始事实；显式ProtocolFrame fact与独立全域Audit已通过synthetic。跨Shard生命周期和开放边界由N30.5 Mandatory Derived从同一Canonical generation确定性重建。 |
| N30.5 Derived/N29整合 | NotStarted | — |
| N30.6 Query/MCP/导出 | NotStarted | — |
| N30.7 LTS-1 | NotStarted | — |
| N30.8 Profiler Session | NotStarted | — |
| N30.9 LTS-2 | NotStarted | — |

后续每阶段记录：源码HEAD、测试RED/GREEN证据、工具SHA-256、输入身份、事件/关系计数、内存/磁盘/耗时、失败项和回滚提交。

## 3. N30.1 Schema 与原子存储

### 实现结果

- 新增 Trace Session Store、Canonical 和 Derived schema 1。
- 新增完整 Session 状态模型、Source Identity、Manifest 和 Shard 描述。
- Shard 使用相对路径、大小和 SHA-256 校验；拒绝目录逃逸路径。
- `.building.<uuid>` 在发布前不可作为 Trace 查询。
- 首次发布把 building generation 原子切换为最终 Session。
- 相同 source identity 可发布后续 immutable generation，并原子更新 `CURRENT`；上一成功 generation 保留。
- 不同 source identity 拒绝覆盖现有 Session。
- 完成 Session 可在原 stream 暂时不可用时继续查询；需要重建时通过独立的完整 SHA-256 校验 source identity。
- 同尺寸原地篡改 Shard 可被 SHA-256 校验识别。

### TDD 证据

RED：

1. 首次测试因 `TracyTraceSessionStore.hpp` 尚不存在而编译失败。
2. 增加第二 generation 用例后，旧发布逻辑返回 `session_output_exists`，用例按预期失败。
3. 增加强 source identity 用例后，因 `VerifyTraceSessionSourceIdentity` 尚不存在而编译失败。

GREEN：

```text
Test project C:/CodeProjects/GodotProjects/tracy-0.13.1-n30-long-trace-session/build-n30-query
    tracy-gpu-analysis             Passed
    tracy-trace-session-store      Passed
100% tests passed, 0 tests failed out of 2
```

覆盖用例：

- schema 常量与默认目录命名。
- building 状态查询拒绝。
- Canonical Frame Shard 写入和 SHA-256 校验。
- 强制 Derived/Audit 未完成时禁止发布。
- 首次发布、读取和完整校验。
- 相同 source 的第二 generation 切换与旧 generation 保留。
- 相同大小 source 替换的强身份不匹配。
- 相同大小 Shard 内容篡改。
- 既有 N29 GPU Analysis 回归。

### 构建环境诊断

首次配置使用新 build 目录自行下载 Capstone，Windows 路径长度超限。对照 N29 的 CMakeCache 后确认根因是遗漏已有 CPM source cache。重新配置为：

```text
CPM_SOURCE_CACHE=C:\CodeProjects\GodotProjects\tracy-0.13.1-n29-gpu-analysis\build-n29-deps
```

之后配置、编译和测试正常；没有修改或复制 N29 的依赖缓存。

### 延后至 N30.3 的持久性项目

- 当前文件提交具备临时文件、校验和与原子 rename；完整故障注入和恢复矩阵在 N30.3 执行。
- `FlushFileBuffers` 级耐久性、孤立 generation 清理和杀进程恢复在 Checkpoint/Recovery 实现时统一完成。

## 4. N30.2 Source Inventory 与容量预检（通过）

### 已完成的有界 Journal Inventory

- `ScanJournal` 增加提交后 Record visitor；visitor 只接收已经通过 Header、Payload、Trailer 和全部 CRC 的记录。
- Inventory 将 `maxCollectedRecords` 固定为 0，不在内存保留逐 Record 元数据。
- 精确统计 SessionBegin、ClientToServer、ServerToClient、Checkpoint、SessionEnd 和 Diagnostic 的数量、payload bytes 与 committed bytes。
- 保存 Protocol、Capture Session ID、committed revision、valid prefix、CRC、完整/降级状态和明确质量原因。
- 完整 SHA-256 覆盖原始文件全部字节，包括 committed prefix 之外的尾部；committed 统计只覆盖验证通过的 prefix。
- Canonical、Derived 和 temporary 使用整数、溢出保护的保守空间估算。
- 容量预检执行 256 GiB Session 上限和 `max(64 GiB, volume 10%)` 安全预留。
- Scan 与 Hash 两个阶段均提供单调 byte progress；诊断工具每5秒输出一次。
- 对全部 `ClientToServer + CompressedFrame` 顺序执行Protocol 90 LZ4流式解码，严格验证每个事件的QueueType、固定大小、可变payload长度和帧结束边界。
- 精确保存每个QueueType的事件数、编码字节和可变payload字节；汇总为17个稳定数据域，未知域单独返回`other`。
- 每个压缩帧先在临时计数器中完整验证，再事务性合并；损坏帧不会把半帧计数写入Inventory。
- 解析Schema 1 `SessionEnd` payload，保存close reason与client/server累计字节。`CaptureComplete`/`LocalShutdown`可完整，其他原因按稳定quality reason标记source degraded。
- Inventory持久化时对QueueType明细、数据域明细、总事件数和总编码字节做交叉守恒校验；缺行或总量不一致会拒绝加载。
- 可选run输出将每个已提交Journal Record的位置、序号、到达时间、payload大小、flags和类型写入固定40-byte记录；Dictionary、Source/Callstack、FrameImage Blob、GPU Reference Dictionary和GPU Catalog Blob另写依赖定位run。
- run采用64 MiB目标、固定Header、相对路径、SHA-256和原子rename；Inventory manifest保存每个run的记录范围、数量、大小和hash。run损坏、越界路径或总数缺失会被拒绝。

### TDD 证据

RED：

1. Inventory 接口尚不存在时，测试因缺少 `TracyTraceSessionInventory.hpp` 编译失败。
2. Progress API 尚不存在时，测试因缺少 `TraceSessionInventoryPhase` 和 progress callback 编译失败。
3. Protocol frame parser尚不存在时，测试因缺少`TracyTraceSessionProtocolInventory.hpp`编译失败。
4. LZ4连续解码器尚不存在时，测试因缺少`TraceSessionProtocolDecoder`编译失败。
5. 域分类和CaptureEnd字段尚不存在时，编译按预期失败。
6. immutable run接口尚不存在时，测试因缺少run options、manifest和验证API而编译失败。

GREEN 覆盖：

- clean terminal journal。
- committed revision 和按方向 bytes/count。
- recoverable truncated tail。
- 10,000 records 下 retained metadata 恒为0。
- Inventory 原子落盘和 round-trip。
- Session 256 GiB 上限、64 GiB/10% volume reserve。
- Scan/Hash progress 单调且抵达 source size。
- 固定事件、16位/32位可变payload和截断帧边界。
- LZ4连续字典解码和压缩记录长度不匹配。
- Journal→CompressedFrame→QueueType端到端统计。
- Frame、Job、Scheduling、CPU Memory、GPU Memory、GPU Catalog和Dictionary域分类。
- CaptureComplete与ProtocolMismatch关闭原因质量映射。
- QueueType明细、域明细和总量持久化守恒校验。
- 小目标容量下强制Journal/Dependency多run、manifest round-trip、全记录覆盖、SHA-256验证和损坏拒绝。
- N30.1 Store 与 N29 GPU Analysis 联合回归。

```text
tracy-gpu-analysis             Passed
tracy-trace-session-store      Passed
tracy-trace-session-inventory  Passed
100% tests passed, 0 failed
```

### 真实30分钟输入

输入：

```text
C:\Users\Admin\Documents\JN-Unity-T3\N29-Autonomous-30m\Run-20260830-220012\Admin-HighEvidence-30m.tracy-stream
```

结果：

| 项目 | 数值 |
|---|---:|
| Source bytes | 19,122,822,908 |
| Source SHA-256 | `2AC46C53257CE9027EC67E098FC15070FB911243F6CD311A166EB97547848068` |
| Valid committed bytes | 19,122,822,908 |
| Committed revision / records | 499,362 |
| Client records | 193,144 |
| Client payload bytes | 19,078,853,869 |
| Server records | 304,425 |
| Server payload bytes | 3,976,985 |
| Compressed protocol frames | 193,141 |
| Protocol events | 2,284,723,486 |
| Capture end | CaptureComplete (`3`) |
| Source state | complete / not degraded |
| Estimated Canonical | 22,947,387,490 bytes |
| Estimated total build | 51,631,621,852 bytes |
| Required available incl. reserve | 461,259,618,422 bytes |
| Capacity | accepted |
| Protocol-aware peak Working Set | 14.66 MiB |
| Protocol-aware peak private/paged | 8.06 MiB |
| Protocol-aware墙钟 | 174.235秒 |

最终域级Inventory为4,244 bytes，SHA-256：

```text
1C811C88EA5989F136D93608E198FC21E4C2C122FB565A4509FF9B2DCDB721AD
```

保存位置：

```text
C:\Users\Admin\Documents\JN-Unity-T3\N30-Inventory\
```

30分钟协议事件域计数：

| 域 | 事件数 |
|---|---:|
| CPU Zone | 976,556,944 |
| Job/Gfx | 432,271,644 |
| Scheduling/Context Switch | 279,606,178 |
| Sampling/Hardware Sample | 206,187,865 |
| GPU Zone | 147,490,405 |
| Relation | 125,513,195 |
| GPU Memory Reference | 74,883,055 |
| Dictionary | 20,261,690 |
| CPU Memory | 7,449,296 |
| Source/Callstack | 6,377,313 |
| GPU Catalog | 5,069,734 |
| Message/Plot/Lock | 1,487,900 |
| Frame/FrameImage | 1,004,931 |
| Script Runtime | 563,231 |
| I/O | 104 |
| Control | 1 |
| Other | 0 |

短Trace验证中，Inventory得到的`protocol_events=142,886,374`、`protocol_frames=9,857`，与传统Full Worker转换报告逐项相等。30分钟stream完整解压后`other=0`，未发现非法QueueType、压缩字典错误或事件边界错误。旧转换在42.6%发生的40.83 GiB内存失败不是该位置的源stream损坏。

### 真实30分钟 immutable runs

输出：

```text
C:\Users\Admin\Documents\JN-Unity-T3\N30-Inventory\
Admin-HighEvidence-30m-with-runs.inventory
Admin-HighEvidence-30m-with-runs.inventory.runs\
```

| 项目 | 结果 |
|---|---:|
| Journal runs | 1 |
| Journal records / bytes | 499,362 / 19,974,512 |
| Dependency runs | 20 |
| Dependency records / bytes | 32,420,178 / 1,296,807,760 |
| Total indexed records | 32,919,540 |
| Total run bytes | 1,316,782,272 |
| Inventory SHA-256 | `68A9ECF6DFD7B7BF0321DD4FEB56524AE82F64CE283BF559515172082C598589` |
| Wall clock | 187.091秒 |
| Peak Working Set | 157.43 MiB |
| Peak private/paged | 180.51 MiB |

峰值出现在64 MiB run最终封存时的临时payload复制；即使保留该待优化复制，内存仍由run target硬界定，未随22.8亿协议事件线性增长。run文件全部以相对路径发布，原始stream保持只读且SHA-256未变化。

### 尚未完成，不能提前通过 N30.2

以下内容按其状态所有权进入后续阶段，不再阻塞N30.2：

- 协议事件精确时间解码后的迟到分布属于N30.3可序列化decoder/checkpoint状态；当前明确不使用Journal到达时间伪装语义时间。
- producer在JN质量事件中声明drop时的逐域映射属于N30.4 Canonical domain decode；N30.2已完成SessionEnd关闭原因映射。
- Inventory/Canonical统一build-state、GracefulCancel/Resume属于N30.3。

## 5. N30.3 Canonical 与 Checkpoint（进行中）

### 已完成基础

- `TraceSessionProtocolDecoder`可导出和恢复最近64 KiB LZ4字典。
- 测试使用两个连续压缩帧，第二帧依赖第一帧字典；新decoder从checkpoint恢复后精确得到第二帧事件。
- Canonical sink不构建传统Worker；逐记录读取stream，经共享Protocol parser解压后复制事件原始编码字节。
- 非压缩的SessionBegin、ServerToClient、Checkpoint、SessionEnd和Diagnostic以transport record保留，避免新Session丢失服务端请求及录制控制事实。
- Canonical record保存kind、QueueType/RecordType、数据域、flags、source sequence、Journal到达时间、protocol frame ordinal和原始payload。
- Shard只在完整Journal record边界提交；每个Shard后提交checkpoint。
- Checkpoint保存source sequence、下一source offset、protocol frame ordinal、累计协议事件数、累计transport record数、LZ4 dictionary和前一checkpoint hash。
- Canonical开始前重新执行source强身份验证，防止Inventory后同尺寸替换源文件。
- Journal scanner新增`Stopped`状态，只在完整Record Header、Payload、Trailer和全部CRC校验通过后响应停止请求。
- 第一次取消会封存当前Shard、提交Checkpoint、原子更新manifest为`CancelledResumable`；不会发布Session。
- 恢复前重新校验source强身份、全部已提交Shard及Checkpoint链；从最后source sequence继续，已提交Shard不重写。
- 当前实现为正确性优先：恢复时重新顺序验证source前缀，但跳过已提交记录的payload读取和Canonical重建；后续可由持久Inventory run优化seek，不改变恢复语义。
- 同一全局segment内按17个稳定域分别写Shard；Frame、CPU/GPU Zone、Job、Memory、Catalog、Sampling等不再混在单一`protocol`文件，所有域Shard共享该segment的source/time边界。
- `VisitTraceSessionCanonicalShard`作为后续Derived、Query和Exporter的共享有界Reader，逐Shard验证SHA-256、header、record framing、domain、source range和record count。
- `TraceSessionWriterLease`使用原子目录、PID、process creation time、lease generation和heartbeat保证单writer；活跃writer被拒绝，malformed/dead lease被隔离后恢复。
- Canonical默认soft/hard内存门禁为12/16 GiB；每次追加前先做整数安全预算检查，超过硬上限时标记`InvalidCapacity/resource_limit`并保留上一个已提交Checkpoint。
- Shard提交后释放各域buffer capacity，避免不同域的历史峰值在长录制中永久累积。
- 每条Protocol Canonical record保存当时的`ThreadContext`、`has_semantic_time`和原始Tracy TSC；Journal monotonic ns继续单独保存，二者不混用。
- 中央`TryGetTraceProtocolEventTime`按QueueType和正式POD结构提取CPU语义时间；`GpuTime`等只有GPU原始时钟、无法作为CPU时间的事件明确不标记。
- Checkpoint额外保存当前ThreadContext，取消发生在一个压缩frame之后、恢复到下一个frame时仍能正确关联线程。
- 每次Shard事务前检查磁盘可用空间，默认保留`max(64 GiB, volume 10%)`；不足时进入`InsufficientDisk/insufficient_disk`，保留上一个Checkpoint，不写不安全Shard。

### TDD证据

RED：

1. decoder尚无`ExportDictionary/RestoreDictionary`时编译失败。
2. Canonical尚无cancel callback、三态BuildResult与resume状态时，取消/恢复测试按预期编译失败。

GREEN：

- 连续字典压缩帧恢复。
- 小Shard阈值强制多Shard。
- decoded protocol events + non-compressed transport records总数守恒。
- 每个Canonical segment存在checkpoint。
- Canonical和Checkpoint文件大小、manifest及SHA-256全部通过`VerifyTraceSession`。
- 在第一个连续压缩帧后取消，manifest进入`CancelledResumable`，且已提交Shard可验证。
- 从Checkpoint恢复LZ4连续字典并解析依赖第一帧字典的第二帧；累计事件和transport记录无重复、无丢失。
- 恢复后原有Shard ID与SHA-256完全不变，只追加新Shard。
- 同尺寸source内容替换被完整SHA-256拒绝，返回`source_sha256_mismatch`。
- 已提交Checkpoint追加损坏字节后，恢复返回`session_shard_size_mismatch`，不会越过损坏点猜测继续。
- Journal停止测试证明`validSize`严格等于第二个完整Record末尾，且停止前缀可恢复、不伪装为完整Session。
- 17域分片的总记录数与源Inventory守恒；Reader逐项复现Frame=1、Job=1、Dictionary=2、Control=2测试事实。
- 只有3字节的畸形Canonical payload返回`canonical_shard_record_header_truncated`。
- 同一Session的第二个writer返回`session_writer_lease_active`；release后可重新获取，malformed stale lease可安全隔离恢复。
- 人工64-byte hard-memory fixture在已有Checkpoint后触发`canonical_memory_hard_limit`，manifest明确进入`InvalidCapacity/resource_limit`且保留Checkpoint。
- 取消/恢复跨两个LZ4 frame时，第二帧Job事件继承Checkpoint中的ThreadContext=77；FrameVsync原始TSC=123456逐字保持。
- 注入式磁盘探针在第二个Shard前耗尽reserve，返回`canonical_disk_pressure`和`InsufficientDisk/insufficient_disk`，第一个Checkpoint保持可验证。

阶段回归：

```text
tracy-gpu-analysis             Passed
tracy-trace-session-store      Passed
tracy-trace-session-inventory  Passed
tracy-stream-journal           Passed
100% tests passed, 0 failed
```

一次联合回归在N29 GPU Analysis固定temp目录发布时出现Windows `Access is denied`，随后可稳定复现为测试隔离与generation发布的组合缺陷：测试忽略固定temp根目录清理失败，可能复用上次中断留下的generation；Store又仅以微秒时间命名generation，并以`create_directories`接受已存在目录，最终到目录rename时才失败。修复后测试使用PID+单调nonce独立根目录；生产generation使用时间、PID和进程内原子序号，并以原子`create_directory`占位；Windows目录发布对短暂sharing/access冲突执行有界重试，已存在目标则明确返回`store_generation_collision`。同一CTest连续5次通过，不再把该问题归类为“不可复现”。

### 后续派生阶段负责

- GPU calibration、staged dictionary/payload和各域生命周期不会复制进Canonical转换checkpoint；N30.5从Canonical原始事实构建可重建索引。
- 跨Shard Zone、Job、Allocation、Resource、I/O状态由Mandatory Derived保存，并通过Canonical事件计数与Final Audit校验。
- 磁盘压力运行中暂停；内存硬停止与writer lease已经通过。
- Converter级第一次/第二次Ctrl+C交互与进程级故障注入（Canonical API层的安全停止/恢复已经通过）。

## 6. N30.4 全 Canonical 域（通过）

### 已完成基础

- Canonical不再依赖“每个压缩frame至少有一个事件”的隐含假设；每个压缩Journal record额外生成零payload的`ProtocolFrame`事实。
- `ProtocolFrame`保存source sequence、frame ordinal、Journal时间和ThreadContext，即使原Protocol frame为空也能在Canonical中被独立计数和审计。
- `TraceSessionCanonicalAudit`从已落盘Shard重新读取，不复用构建期计数器。
- Audit逐项重算QueueType事件数、encoded bytes、variable payload bytes、17域统计、Protocol frame数、transport record数和semantic-time覆盖。
- Audit要求所有结果与Source Inventory完全守恒；缺少一个合法Shard也会返回明确count/domain mismatch，不允许发布为完整。
- 新增Protocol 90完整QueueType矩阵：从`0`到`NUM_TYPES-1`逐一构造合法零payload事件，要求每种类型恰好出现一次、全部落入稳定域，域总数与事件总数严格相等。
- Canonical只保存可直接证明的协议事实；生命周期、inclusive集合等派生状态不写回Canonical，也不与Canonical并列成为第二事实来源。

### TDD证据

RED：尚无`TraceSessionCanonicalAudit`和`AuditTraceSessionCanonical`时，全域守恒测试按预期编译失败。

GREEN：

- 2个连续压缩frame、5个Protocol事件和2个非压缩transport record全部守恒。
- Canonical总记录额外包含2个显式ProtocolFrame事实，空frame语义不再依赖事件推断。
- protocol frame/event/encoded bytes/transport records与Inventory逐项一致。
- QueueType和17域的count/encoded/variable payload统计完全一致。
- Protocol 90的全部QueueType逐项各计数1次，域分区总数与`NUM_TYPES`完全一致。
- semantic-time覆盖只统计可证明拥有CPU语义时间的事件。
- 从manifest删除Frame shard后，`AuditTraceSessionCanonical`明确失败，不使用其他域推测补齐。
