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
| N30.5 Derived/N29整合 | Passed | 全域不可变索引、Canonical GPU→N29 derived、pointer生命周期、强制索引门禁和Final Audit已通过synthetic；失败不会发布Session。 |
| N30.6 Query/MCP/导出 | InProgress | Query 1.34已直接打开Session GPU derived；generation固定、能力门禁、构建状态、有序Canonical Reader、Frame、Job、CPU Zone和Memory语义索引已完成，其余域的分页Reader及局部导出仍在实施。 |
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

## 5. N30.3 Canonical 与 Checkpoint（通过）

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
- `VisitTraceSessionCanonicalOrdered`按Checkpoint分段合并同一segment内的域Shard，并按`source sequence + protocol frame ordinal + frame offset`恢复全局顺序；只持有一个segment，内存不随录制总时长增长。
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
- 有序Reader无需调用方排序即可逐字恢复跨域事件顺序，且每个Canonical fact恰好访问一次；每段的数据Shard和Checkpoint均在暴露记录前完成校验。

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
- 每个Protocol事件额外持久化其解压frame内的原始byte offset；`source sequence + frame ordinal + frame offset`构成跨域全序键，避免按域分片后丢失同一frame内的真实先后关系。
- Canonical只保存可直接证明的协议事实；生命周期、inclusive集合等派生状态不写回Canonical，也不与Canonical并列成为第二事实来源。

### TDD证据

RED：尚无`TraceSessionCanonicalAudit`和`AuditTraceSessionCanonical`时，全域守恒测试按预期编译失败。

GREEN：

- 2个连续压缩frame、5个Protocol事件和2个非压缩transport record全部守恒。
- Canonical总记录额外包含2个显式ProtocolFrame事实，空frame语义不再依赖事件推断。
- protocol frame/event/encoded bytes/transport records与Inventory逐项一致。
- QueueType和17域的count/encoded/variable payload统计完全一致。
- Protocol 90的全部QueueType逐项各计数1次，域分区总数与`NUM_TYPES`完全一致。
- Frame、Dictionary、Job等交错事件从不同域Shard重新归并后，与原始Protocol frame顺序逐项一致。
- semantic-time覆盖只统计可证明拥有CPU语义时间的事件。
- 从manifest删除Frame shard后，`AuditTraceSessionCanonical`明确失败，不使用其他域推测补齐。

## 7. N30.5 Mandatory Derived与N29整合（通过）

### Canonical GPU Reader

- 新增`LoadTraceSessionGpuCanonicalData`，输入只允许同一Session manifest generation的Canonical Shard，不读取原始stream，也不构建完整`Worker`。
- 从Control Shard中的原始`WelcomeMessage`恢复`timerMul`与`initBegin`，时间换算使用与Worker相同的`(tsc-baseTime)*timerMul`语义；缺失、非法或冲突的Welcome阻止GPU派生。
- GPU Catalog payload、control和batch重新执行generation、sequence、schema、record width、payload size与checksum验证。
- Catalog Resource、Allocation、View、Logical、Part、Relation、VG、Range、Detailed Evidence和String均使用同一解析入口；转换器制造的gap或未消费payload直接失败。
- GPU Reference支持direct use及ResourceSetV2定义/展开；Pass、Use和End保留thread、frame、taxonomy、usage、command list及dropped计数。
- 连接结束时未闭合Catalog generation被忠实标记为无效，不伪造成Complete。
- `ResolveJnGpuCatalogGenerationData`成为传统Worker和Session共用的纯数据解析器；View、Logical、VG、Range、Relation和DetailedEvidence使用相同的资源生命周期区间解析，不维护两套易漂移语义。
- pointer token在同一地址Destroy后再次Create时按事件时间解析到不同`GpuResourceId`，歧义区间返回unresolved而不猜测。
- N29 Store writer新增显式algorithm root入口；Legacy `.tracy` sidecar继续使用原路径，Session直接写入当前generation的`derived/gpu-resource-analysis/<schema>/<algorithm>`。
- `BuildTraceSessionGpuAnalysisDerived`直接消费Canonical GPU事实并构建N29 Snapshot/索引，不写独立Raw GPU sidecar，也不重读stream。
- 新增全域`session-index`：每个Canonical事件Shard对应一个immutable索引文件，逐记录保存source sequence、frame ordinal/offset、Journal时间、语义时间、线程、类型和kind；文件及manifest独立SHA-256并原子发布。
- Mandatory Derived统计Protocol Event、显式ProtocolFrame、Transport Record和17域总数；Final Audit重新读取Canonical与索引文件，不复用构建期计数器。
- Final Audit要求`Inventory source count = Canonical count = Derived source count`，并验证GPU Store的source SHA/size、Complete状态及Resource/Allocation/Pass数量。
- `tracy-stream-convert`生产入口已改为只生成`.jn-trace-session`；`-o`只表示Session目录，`.tracy`输出会明确拒绝。旧完整Worker转换只在`JN_STREAM_CONVERT_DEV_TOOLS`下以`tracy-stream-convert-legacy`保留作差分Oracle。
- 新Converter串联Inventory、容量预检、Canonical、Mandatory Derived、Final Audit和原子Session发布；默认输出无需格式参数，`--status`、`--resume`和`--restart-generation`已接入。

### 当前TDD证据

- Synthetic Session包含真实Welcome、Catalog Generation Begin/Batch/End、同一pointer的Destroy/Create复用以及Pass→Resource→End链。
- 读取结果恢复两个资源生命周期；复用前后的View分别精确解析为ResourceId 10和11，pointer token被消解。
- `timerMul=2`、`initBegin=100`下，原始TSC 110/111/112/113严格换算为20/22/24/26 ns。
- 两个Catalog payload各消费一次，batch数与unresolved payload计数分别为2和0。
- 同一Synthetic Session成功生成N29 GPU derived generation；Store manifest为Complete，资源数2、Pass数1，且Session目录中不存在独立Raw sidecar。
- Synthetic的全部Canonical记录均进入带校验和的全域索引；独立Final Audit数量完全一致后Session才切换为Complete并通过`IsTraceSessionQueryable`。
- N29 GPU Analysis、Session Store和Session Inventory联合回归通过。

### 尚未完成

- Job/Allocation等生命周期的语义型二级索引；当前全域磁盘索引已保留构建这些索引所需的精确顺序、时间和位置，但不能提前宣称Query语义接口已完成。
- Converter在Inventory/Derived阶段的细粒度Ctrl+C checkpoint与持久化ETA。

## 8. N30.6 Query/MCP 与 SessionTraceSource（进行中）

### 已完成的第一条生产查询链

- Query API从`1.33.0`升级为`1.34.0`，协议schema与三份coverage文档同步升级。
- `TraceSourceKind`新增`Session`，Query能力声明新增`source_kind=session`。
- `SessionManager`只接受已经原子发布并通过`IsTraceSessionQueryable`的`.jn-trace-session`目录；普通目录、伪装成`.tracy`的目录和`.building`均不能作为Trace打开。
- `GpuAnalysisStoreReader::OpenAt`可以从Session generation的mandatory derived algorithm root读取N29索引，并强制校验source SHA-256和size。
- `GpuAnalysisTraceSource::OpenSessionIfReady`直接打开Session GPU派生层，不实例化完整Worker；Legacy `.tracy + .jn-gpu-resource-analysis`路径保持不变。
- Query的GPU Reader cache识别Session目录，固定读取同一个manifest generation的`derived/gpu-resource-analysis`。
- `gpu.memory.peak`及已有N29 GPU分页查询复用同一Store Reader；Session不会生成第二份Raw GPU sidecar，也不会回退到完整内存Snapshot。
- Query打开Session后固定该次`manifest generation`和GPU Store Reader；即使之后`CURRENT`切换，新旧请求也不会跨generation混读。
- Session能力区分`present`、`indexed`和`queryable`：Canonical已有事实但尚无磁盘语义Reader的域明确返回`CAPABILITY_UNAVAILABLE`，禁止伪装成空结果或回退完整Worker。
- Session打开为O(manifest)：发布前Final Audit执行全量强校验；打开时不重算全部Shard SHA，实际读取的immutable shard/page仍在首次访问时校验大小与SHA-256。
- `session.build.status`可以在不打开Trace数据的情况下读取`.building`或已发布Session的state、generation、source identity、Canonical shard/bytes、当前Shard、最后Checkpoint、已处理source record、阶段进度、mandatory derived、Final Audit和reason；路径仍受`--allow-root`约束。
- Inventory支持在完整Journal record边界响应Ctrl+C并返回`cancelled_safe_restart`；不会发布部分Inventory。Derived使用`stop_token`，第二次Ctrl+C只等待安全收尾，不走进程内强杀。
- 多个同名`.building`目录不再依赖目录枚举顺序：按source大小/强SHA筛选，再按已提交source record、manifest时间和目录名确定恢复候选。
- Frame域已建立第一个非GPU磁盘语义索引：从Canonical Dictionary和Frame Shard恢复FrameName、连续Frame、显式Begin/End和Vsync FrameSet，使用与Worker一致的Welcome时间换算；最后一个连续Frame只在存在可证明的全局最后语义时间时闭合。
- `frame.sets`、`frame.list`、`frame.get`、`frame.statistics`、`frame.outliers`和`frame.range_mapping`直接读取Frame索引，不实例化Worker；`frame.identity`依赖JN关联帧语义，当前不声明为可查询。
- Frame索引包含source/generation强身份、独立文件SHA-256、保留字段和计数审计；Final Audit同时核对Canonical semantic-time事件数和Frame set/frame/complete计数。
- Job域新增`job-index/1/exact/jobs.bin`：从有序Canonical事实恢复Job Type、Schedule、Config、Dependency、Stage、关联Frame和SiteReuse Callsite；时间统一使用Welcome换算，Schedule/Ready/Queue/Slice/Complete/Wait及其子状态沿用传统Worker语义。
- Job调用栈严格复现Tracy协议的两类状态：普通`Callstack`按线程消费，`CallstackSerial`可跨无关事件保持到对应Job/Callsite消费者；Sampling字典栈也参与全局Callstack ID去重，避免Job的`stack_ref`与后续Source/Callstack索引错位。
- Job索引包含source SHA-256、source size、固定manifest generation、文件SHA-256及Type/Schedule/Config/Dependency/Stage计数；Final Audit逐项与Inventory的QueueType计数核对，不能用已构造Job数量替代源事件守恒。
- `job.search/get/dependencies/critical_path/statistics`已从Session Job Reader读取且不会回退完整Worker；`job.gfx`仍保持不可查询，直到Gfx Dispatch/Entity/Link语义Reader完成。
- CPU Zone域新增`cpu-zone-index/1/exact/cpu-zones.bin`：只在构建期保留各逻辑线程的活动Zone栈，闭合Zone立即写固定宽度磁盘记录；父子关系、child count、self time和开放捕获边界均由Canonical事实重建。
- CPU Zone时间严格复现Worker的共享`m_refTimeThread`语义；Plot、非serial GPU CPU时间和Fiber切换会推进同一线程时钟，避免跨域事件插入后Zone时间漂移。
- 标准/动态SourceLocation、延迟StringData、ZoneName、ZoneText、ZoneColor、普通Callstack、Serial Callstack和SiteReuse Callsite均已接入；Callsite晚于Zone闭合到达时原位修补已提交的固定记录。
- `zone.cpu.search/get/tree/statistics/flamegraph`已从Session CPU Zone Reader读取；最后未闭合Zone返回`complete=false/end=null`，不会伪造捕获结束时间。
- 快速Session打开只校验CPU Zone文件头、身份和大小；Final Audit独立重算`cpu-zones.bin`完整SHA-256并核对Inventory Begin/End守恒，同尺寸篡改不能被发布。
- Memory域新增`memory-index/1/exact/memory.bin`：每个Memory Pool使用独立顺序work file，已完成allocation立即持久化，free/discard只原位补齐生命周期；构建期内存仅保留各池当前存活地址表，不随历史alloc/free事件总数增长。
- Memory Reader恢复默认池与命名池、地址、大小、alloc/free时间、原始线程、Callstack和开放生命周期；地址复用始终建立新的事件代次，不覆盖旧生命周期。
- Memory串行时间严格复现Worker的共享`m_refTimeSerial`；Lock serial、GPU serial、Memory、Job Wait/Schedule stack和I/O stack消费者保持同一协议状态，避免插入其他域事件后Memory时间漂移。
- `JnMemAllocCallsiteNamed`使用已注册SiteReuse Callsite的全局Callstack ID；传统`MemAlloc/FreeCallstack*`保留逐事件栈。命名池StringData允许延迟到达，最终按稳定名称、再按native pool id排序。
- `memory.pools/events/get/active_at_time/frame_snapshot/diff/callstack_tree/leak_candidates`现由Session Memory Reader提供，不实例化Worker；`memory.gpu`只在至少一个`GPU D3D12 `命名池存在时声明可查询。
- `memory.bin`保存source SHA-256、source size、固定manifest generation、Pool/Event/Active计数与独立SHA-256；Final Audit核对五类alloc、四类free、两类discard的Inventory源事件守恒，并拒绝同尺寸payload篡改。
- Sampling域新增`sampling-index/1/exact/samples.bin`：构建时按native thread使用独立顺序work file，普通Sample与ContextSwitch Sample立即落盘；内存仅保留线程文件状态、Callstack内容字典和Sample Dictionary，不保存与总样本数成比例的事件容器。
- Sampling Reader严格保持Worker的输出顺序：native thread升序，每线程先普通Sample、再ContextSwitch Sample；时间复用`m_refTimeCtx`语义，Sample、ContextSwitch Sample、ContextSwitch和ThreadWakeup共同推进同一delta时钟。
- `CallstackPayload`与`CallstackSampleDictionary`进入统一内容寻址Callstack ID空间；普通Sample消费pending callstack，Ref Sample只接受已经声明的dictionary id，未知、重复或协议状态冲突均阻止构建。
- `sample.list`现由Session Sampling Reader直接分页扫描固定宽度记录，不实例化Worker；能力只声明已经实现的`sample.list`，Ghost Zone、Flamegraph和Symbol Statistics仍保持不可用，禁止过度宣称。
- `samples.bin`保存source强身份、generation、普通/ContextSwitch Sample、dictionary与Callstack payload计数；Final Audit执行完整SHA-256并与Inventory四类Sample QueueType逐项守恒。
- Scheduling域新增`scheduling-index/1/exact/scheduling.bin`：每个native thread和CPU使用独立顺序work file，切入立即追加，切出只原位补齐End/Reason/State；构建期内存仅保留每线程/每CPU最后一个开放区间和pending wakeup。
- `ThreadWakeup`、`ContextSwitch`以及两类Sample共享Worker的`m_refTimeCtx` delta时钟；wakeup时间/CPU和实际run时间/CPU分别保存，跨CPU唤醒不会被误写成实际运行CPU。
- 线程区间严格恢复Wakeup→Start→End、wait reason/state和开放边界；CPU区间独立恢复实际占用线程，外部线程压缩索引按Worker首次切入顺序生成。
- `context_switch.range/thread/statistics`和`cpu.timeline`直接读取Scheduling Reader，不实例化Worker；`thread`、CPU topology和CPU usage尚未具备磁盘语义Reader，仍保持不可查询。
- Final Audit对`scheduling.bin`执行完整SHA-256，逐项核对源`ContextSwitch`与`ThreadWakeup` QueueType计数；派生线程/CPU区间计数不能替代源记录守恒。

### TDD证据

RED：

- 尚无`GpuAnalysisStoreReader::OpenAt`时，Session GPU Reader测试按预期编译失败。
- 尚无`OpenSessionIfReady`和`TraceSourceKind::Session`时，SessionTraceSource测试按预期编译失败。
- Frame Canonical fact存在但磁盘语义Reader尚未实现时，测试按预期失败于`Session advertises Frame only after its disk-backed semantic reader is ready`。
- Job Canonical fact存在但磁盘语义Reader尚未接入时，测试按预期失败于`Session advertises Job only after its disk-backed semantic reader is ready`。
- Memory Canonical fact存在但磁盘语义Reader尚未接入时，测试按预期失败于`Session advertises Memory only after its disk-backed semantic reader is ready`。
- Sampling Canonical fact存在但磁盘语义Reader尚未接入时，测试按预期失败于`Session advertises Sampling only after its disk-backed semantic reader is ready`。
- Scheduling Canonical fact存在但磁盘语义Reader尚未接入时，测试按预期失败于`Session advertises Context Switch only after its disk-backed semantic reader is ready`。

GREEN：

- 发布Synthetic Session后，底层Reader直接得到2个Resource和1个Pass。
- `OpenSessionIfReady`返回`source_kind=session`、正确source fingerprint，且`WorkerLoaded=false`。
- Query 1.34通过`trace.open`直接打开Session目录，再执行`gpu.memory.peak`成功；结果明确来自Session mandatory N29派生层。
- 构建中的Synthetic Session返回`CanonicalBuilding/published=false`；方法注册表、domain coverage和参数schema逐项一致。
- `CURRENT`在`trace.open`后切换到另一generation，既有Trace ID仍从原generation查询成功。
- Synthetic中存在CPU Zone Canonical fact但语义Reader未完成时，能力为`present=true/indexed=true/queryable=false`，`zone.cpu.search`返回`CAPABILITY_UNAVAILABLE`。
- 发布后人工破坏一个未访问Canonical Shard，Session manifest仍可快速打开；第一次实际读取该Shard返回`session_shard_size_mismatch`，证明快速打开没有取消数据完整性门禁。
- 有序Canonical Reader先RED为接口不存在，GREEN后恢复跨域全序且记录数完全守恒。
- Frame GREEN恢复Synthetic `Vsync 9`的2个完整Frame；Welcome换算后的区间为`[12,36]ns`和`[36,40]ns`，Query 1.34的`frame.sets/frame.list`结果与直接Reader一致。
- 提交后篡改`frames.bin`会被`session_frame_file_sha256_mismatch`拒绝，损坏的语义索引不能被发布结果静默使用。
- Job GREEN恢复`SyntheticJob #500`的Schedule=16ns、Ready=20ns、首个Worker Slice=22ns、Complete=32ns、执行时长=8ns；直接Reader和Query `job.search`结果一致。
- Synthetic先注册Sampling字典栈、再注册Job SiteReuse栈；Job必须得到全局Callstack ID 2、Callsite ID 77和`SiteReused` provenance，证明不同调用栈生产者不会串栈。
- 提交后篡改`jobs.bin`会被`session_job_file_sha256_mismatch`拒绝。
- CPU Zone GREEN恢复父Zone`[8,28]ns/self=16ns`、子Zone`[16,20]ns`和开放Zone`start=32ns/end=null`；晚到SiteReuse定义仍关联全局Callstack ID 3，直接Reader与Query 1.34结果一致。
- CPU Zone索引的大小篡改由Reader拒绝，同尺寸内容篡改由Final Audit的完整SHA-256拒绝。
- Memory GREEN恢复默认池3个allocation与1个free：`0xdead`第一代为`[18,22]ns/4096 B`，第二代从24ns开放；`0xbeef`从26ns开放。首帧`[12,36]ns`统计为allocated 13,312 B、freed 5,120 B、end 8,192 B。
- 命名池`GPU D3D12 Texture`恢复1个allocation/free；allocation使用全局Callstack ID 4和SiteReuse Callsite 79，free使用逐事件Callstack ID 5，直接Reader与Query 1.34一致。
- Final Audit在`memory.bin`同尺寸内容被篡改后返回`session_memory_file_sha256_mismatch`。
- Sampling GREEN恢复thread 42的普通Sample=14ns与ContextSwitch Sample=16ns；两者都由Sample Dictionary解析到全局Callstack ID 1，直接Reader和Query 1.34 `sample.list`结果一致。
- Final Audit在`samples.bin`同尺寸内容被篡改后返回`session_sampling_file_sha256_mismatch`。
- Scheduling GREEN恢复thread 42的`[18,28]ns`运行区间和thread 43的`wakeup=22ns/start=28ns/end=36ns`区间；后者保留wakeup CPU 1、实际CPU 0以及`delay_execution/ready`结束状态。
- CPU timeline GREEN恢复CPU 0上thread 42与43的两个连续区间；直接Reader和Query 1.34 `context_switch.range`结果一致。
- Final Audit在`scheduling.bin`同尺寸内容被篡改后返回`session_scheduling_file_sha256_mismatch`。
- Query、MCP transcript、Session、GPU Analysis和N29静态一致性共8项回归全部通过。

### 尚未完成，不能提前通过N30.6

- GPU Zone和FrameImage的语义分页Reader。
- Scheduling当前以线程/CPU固定宽度区域线性扫描；在N30.6完成前仍需增加immutable时间/线程/CPU索引并验证长Trace查询延迟。Thread identity/name、CPU topology和CPU usage磁盘Reader尚未完成。
- Sampling当前以单个固定宽度文件线性扫描时间范围；在N30.6完成前仍需增加immutable时间/线程索引并对长Trace查询延迟做门禁。Callstack frame、符号和SourceLocation磁盘Reader尚未完成，因此Sample目前能返回稳定`callstack_ref`，但不能在Session路径解析到完整符号帧。
- 当前Job Reader已经具备正确语义和强校验，但打开时仍会物化该Session的全部Job DTO；在N30.6完成前必须改为immutable Job shards + 分页/范围读取，不能把当前实现用于宣称长录制内存门禁通过。
- 当前CPU Zone Reader已经避免在打开时物化全部Zone，但时间范围和children查询仍线性扫描单个`cpu-zones.bin`，SourceLocation元数据仍驻内存；在N30.6完成前必须增加immutable时间/父索引并验证长Trace查询延迟，不能据此提前通过查询性能门禁。
- 当前Memory Reader打开时只驻留Pool描述符和名称，但`memory.events`仍按Pool顺序扫描固定记录，Frame Snapshot仍物化与该帧相交的事件；在N30.6完成前必须增加immutable时间/Pool索引并验证长Trace查询延迟。
- Memory allocation/free到CPU Zone的交叉关联尚未接到磁盘CPU Zone Reader；当前Callstack可导航，但`allocation_zone_ref/free_zone_ref`在Session路径仍为空，不得提前宣称跨域Memory证据链完整。
- 局部`.tracy`导出器及开放边界语义。
- Query/MCP全域结果与传统Worker的短Trace逐项差分。

### 2026-08-31 CPU Zone Reader 构建身份与回归

| 工具 | SHA-256 |
|---|---|
| `build-n30-query\Release\tracy-query.exe` | `CF1D09B7050E4B45CC83A10EA24034963130CC4C1F76E052B737A9EDAB5FB038` |
| `build-n30-capture\Release\tracy-stream-convert.exe` | `44A4F7EF292BD60D8A34DF5C65BE904E12127773DEC2B57CFF2130C1ACE6816E` |

`tracy-query --version`：`0.13.2 / tracy-query/1 / schema 1.34.0`。

七项回归：

```text
tracy-query-contract              Passed
tracy-gpu-analysis                Passed
tracy-trace-session-store         Passed
tracy-trace-session-inventory     Passed
tracy-query-version               Passed
tracy-query-doctor                Passed
tracy-gpu-analysis-n29-static     Passed
100% tests passed, 0 failed
```

### 2026-08-31 Sampling Reader 构建身份与回归

| 工具 | SHA-256 |
|---|---|
| `build-n30-query\Release\tracy-query.exe` | `5C4AAAA311C0D539006147F20E10507CB6B10B67049670D9F403E0DE7AE4D85C` |
| `build-n30-capture\Release\tracy-stream-convert.exe` | `F273B497BBFEC64435C5E30747447E217DCDB9097154BD05B2204548D5CDD191` |

`tracy-query --version`保持：`0.13.2 / tracy-query/1 / schema 1.34.0`。

八项回归：

```text
tracy-query-contract              Passed
tracy-gpu-analysis                Passed
tracy-trace-session-store         Passed
tracy-trace-session-inventory     Passed
tracy-query-mcp-transcript        Passed
tracy-query-version               Passed
tracy-query-doctor                Passed
tracy-gpu-analysis-n29-static     Passed
100% tests passed, 0 failed
```

### 2026-08-31 Memory Reader 构建身份与回归

| 工具 | SHA-256 |
|---|---|
| `build-n30-query\Release\tracy-query.exe` | `921FB679CEE6692B5CBBCCDC8C3686EFDDC5ACFFF97FDB418E11ECA5F6C3114A` |
| `build-n30-capture\Release\tracy-stream-convert.exe` | `33F44EE053E98D83DE5223AF7DEFA8A96C4179CE354338D306E340FD2C183513` |

`tracy-query --version`保持：`0.13.2 / tracy-query/1 / schema 1.34.0`。

八项回归：

```text
tracy-query-contract              Passed
tracy-gpu-analysis                Passed
tracy-trace-session-store         Passed
tracy-trace-session-inventory     Passed
tracy-query-mcp-transcript        Passed
tracy-query-version               Passed
tracy-query-doctor                Passed
tracy-gpu-analysis-n29-static     Passed
100% tests passed, 0 failed
```

### 2026-08-31 Context Switch Reader 构建身份与回归

| 工具 | SHA-256 |
|---|---|
| `build-n30-query\Release\tracy-query.exe` | `F55DE8046A0F73202EC42298CCF93A6B0A10D2D8CDF3511292B3ECF87CCF1E89` |
| `build-n30-capture\Release\tracy-stream-convert.exe` | `2A3A806F3246C191784838C5A0DF2AF634EFAFB670EDCC4D011354E8898BDFE5` |

`tracy-query --version`保持：`0.13.2 / tracy-query/1 / schema 1.34.0`。

八项回归：

```text
tracy-query-contract              Passed
tracy-gpu-analysis                Passed
tracy-trace-session-store         Passed
tracy-trace-session-inventory     Passed
tracy-query-mcp-transcript        Passed
tracy-query-version               Passed
tracy-query-doctor                Passed
tracy-gpu-analysis-n29-static     Passed
100% tests passed, 0 failed
```
