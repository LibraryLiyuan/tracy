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
| N30.6 Query/MCP/导出 | InProgress | Query 1.34已直接打开Session GPU derived；generation固定、能力门禁、构建状态、有序Canonical Reader、Frame、Job、CPU/GPU Zone、Memory、Sampling/Hardware Sample、Scheduling、FrameImage、Source/Callsite/Callstack/Symbol、Relation、Runtime Domain、C#/Lua Script、Plot、Message和Lock语义索引已完成，其余域的分页Reader及局部导出仍在实施。 |
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

### 2026-08-31 真实30分钟Runtime前向字符串与GPU内存门禁

Runtime真实数据修复：

- 真实Protocol 90允许`JnScriptFrame/JnScriptStack`先引用字符串句柄，`StringData`响应随后到达；旧Session Reader错误要求定义先于引用，导致`session_runtime_script_frame_string_unresolved`。
- Reader现将Script引用写入固定宽度work流，单次Canonical扫描完成字符串字典后再按原source order解析；未解析引用在扫描结束后仍明确失败。
- 真实30分钟结果：`domain_states=1`、`script_frames=6`、`script_stack_events=563224`、`string_bytes=338`；`runtime-script.bin=27,035,552 bytes`，SHA-256为`a439046a8d5b2a8841ae1bb17da0a6e08b1e7c4907370655a03c5177acc50027`。

GPU阻断证据：

- 旧N30 Session GPU路径仍执行`LoadTraceSessionGpuCanonicalData → BuildGpuAnalysisSnapshotConsuming`，使约27,375,840个Pass、125,513,195条资源引用及Catalog历史同时驻留。
- 真实进程达到`Private=16.27 GiB / Working Set=13.47 GiB`，违反16 GiB硬门禁；已安全取消，未发布GPU generation或最终Session。
- 第一轮修复将Catalog与Pass拆分、ResourceSet写入磁盘字典、Pass按帧归并为≤64 MiB spool page，并以外部排序run生成Resource↔Pass和Frame↔Pass索引。架构静态门禁明确禁止Session路径重新调用全量`JnTraceData + GpuAnalysisSnapshot`链。
- 第一轮真实复测未再展开Pass引用，但Catalog Range payload本身仍使Private达到10.9 GiB，并在N29 6 GiB分析预算处返回`analysis_hard_memory_limit`；未越过16 GiB，也未发布不完整结果。
- 第二轮修复将Range/Subresource从Catalog snapshot外部化：Range在活动Pass内保持精确关联，同时按ResourceId写独立磁盘页。Store新增分页`RangesForResource`；Range事件不去重，避免把重复事实误删。

TDD与回归：

```text
RED:   Session GPU derived仍调用全量JnTraceData/GpuAnalysisSnapshot
GREEN: N29 GPU static architecture gate passed
RED:   Catalog-only snapshot把尚未装载的Pass Range误判为unresolved
GREEN: Catalog-only Resource/Allocation/Range Oracle passed
GREEN: 分页Pass spool与N29 Store Reader的Pass/Inclusive/反向索引等价
GREEN: Trace Session Inventory tests passed
GREEN: 8/8 CTest passed（第二轮真实Range复测前）
```

当前状态：第二轮真实30分钟复测正在执行；在Session完整发布、GPU count/bytes/peak/checksum审计通过前，N30.5仍标记为`InProgress`，不得宣称LTS-1通过。

### 2026-08-31 Job 分页索引与 Scheduling 句柄上限修复

真实30分钟录制继续暴露出两个只会在大规模数据下出现的问题。本节只记录已经由代码、合成测试和真实构建产物证明的事实；真实Session尚未完成Final Audit，不能据此将N30.6或LTS-1标记为通过。

#### Scheduling失败根因

旧实现为每个观察到的ThreadId和CPU Id保持一个长期打开的`.work`文件。真实录制产生约3000个此类文件后，Windows/MSVC进程文件句柄达到上限，Converter返回：

```text
session_scheduling_work_open_failed
```

这不是源Trace损坏，也不是内存超限；根因是实现的打开文件数量随线程/CPU身份基数线性增长。

修复后每个Scheduling generation只打开两个共享顺序文件：

```text
thread-events.work
cpu-events.work
```

线程/CPU运行时状态只保存最后一条记录和对应全局文件偏移；End事件通过固定宽度记录的全局偏移回填。打开文件数量从`O(thread_count + cpu_count)`降为常数2，最终文件仍保持按Thread/CPU可查询的精确区间语义。Schema根目录使用实际`scheduling-index/2/exact`，不再硬编码旧版本。

合成压力测试增加1024个不同ThreadWakeup身份，并验证：

- 构建成功，不触发句柄耗尽；
- 默认分页上限返回100条，不把截断结果伪装成全量；
- Query显式`limit=4`时精确返回4条；
- Session重开后Scheduling Reader保持可用。

#### Job磁盘分页

真实30分钟Job规模已经超过适合一次性装入Query进程的范围：

```text
Job count:       18,578,994
Job stage count: 211,850,865
raw jobs.bin:     9,644,688,477 bytes
```

新增`job-pages/1/exact`派生索引，使用有界外排和固定宽度磁盘页生成：

```text
job-pages.bin: 9,793,320,437 bytes
```

当前已经完成并验证：

- `TraceSource::GetJobCount()`不物化全量Job；
- `TraceSource::ScanJobs(offset, limit)`只读取请求页；
- `TraceSource::GetJob(jobId)`按ID读取目标Job及其直接前置依赖；
- Session `job.search`和`job.get`使用上述分页路径；
- page header、区域布局、文件大小、SHA-256和source identity均在开放Reader前验证；
- resume时已完成的page generation返回`job-pages-reused`，不重复构建。

仍未完成：

- `job.statistics`的分页聚合；
- 大规模`job.dependencies`反向边索引；
- `job.critical_path`的磁盘图算法；
- Frame→Job关联索引；
- 单个超大Job内部Stage的二级分页；
- page builder构建中途的细粒度Cancel/Checkpoint。

因此当前只能证明基础Job search/get已摆脱全量vector，不能宣称全部Job Query达到长录制内存门禁。

#### 当前真实运行状态

当前generation：

```text
n30-1788153528951461-31412
```

已复用或完成：

```text
Canonical shard count:   402
Canonical logical events: 2,284,723,486
Protocol frames:          193,141
Job page index:           Complete
Scheduling build:         DerivedBuilding
Converter resident memory: approximately 270 MiB during Scheduling
```

真实运行尚未发布Session；必须继续通过Scheduling、剩余Mandatory Derived和Final Audit后，才能形成LTS-1正确性结论。

TDD证据：

```text
RED:   Scheduling stress with high identity cardinality exposed per-identity handle growth
GREEN: Trace Session Inventory tests passed with 1024 scheduling thread identities
GREEN: Job count/scan/get paging tests passed
```

### 2026-08-31 真实30分钟派生索引、Source Clock与Scheduling降级语义

固定输入：

```text
Admin-HighEvidence-30m.tracy-stream
source bytes: 19,122,822,908
source SHA-256: 2AC46C53257CE9027EC67E098FC15070FB911243F6CD311A166EB97547848068
source records: 499,362
protocol frames: 193,141
logical protocol events: 2,284,723,486
canonical shards: 402
```

截至本记录，真实Mandatory Derived已经产生并校验的正式数据包括：

| 域 | 结果 | 正式文件大小 | 说明 |
|---|---|---:|---|
| Job | Passed | 9,644,688,477 bytes | 18,578,994 Jobs、211,850,865 Stages；producer峰值缓存1,048,576条 |
| CPU Zone | Passed | 25,496,383,848 bytes | 234,691,471 Zones；源时钟倒退不再伪造duration |
| Source/Symbol | Passed | 201,512,534 bytes | 符号和Callstack磁盘索引 |
| GPU Zone | Passed | 3,683,879,985 bytes | 正式GPU Zone索引已提交并通过SHA审计 |
| Memory | Passed | 约0.11 GiB | allocation生命周期磁盘索引 |
| Sampling | Passed | 约4.61 GiB | 低内存流式构建 |
| Scheduling | 首次失败后已修复、待真实重跑 | - | 源`oldThread=0`连续switch-in被旧状态机误判 |

#### CPU Source Clock Inversion

真实源中存在一个CPU Zone：End的原始TSC比Begin早49,725 ticks。Canonical保持了原始顺序和数值；Tracy传统Worker在Release中也会保存该事实。N30现在返回：

```text
complete=true
timing_valid=false
timing_invalid_reason=source_clock_inversion
duration_ns=null
self_time_ns=null
```

该Zone仍可导航，但不会进入时间统计、Flamegraph、Evidence Graph或matched-run统计。Session发布状态降级为`CompleteSourceDegraded`并记录精确计数。

#### Scheduling Source Gap

真实源首次触发：

```text
CPU已有未闭合运行区间
→ 新ContextSwitch的oldThread=0
→ 同一CPU再次switch-in
```

Tracy传统Worker会保留前一未闭合区间并继续记录新switch-in。N30旧状态机错误返回`session_scheduling_cpu_switch_in_while_running`。修复后：

- 不补造SwitchOut时间。
- 不丢弃任一观测。
- 前一CPU/Thread区间保持`complete=false`、`end_ns=null`。
- 后续可证明闭合的区间仍正常闭合。
- 记录`source_scheduling_gap`计数并使Session成为`CompleteSourceDegraded`。
- Context Switch duration统计只使用有真实End的完整区间。

Synthetic回归覆盖同一CPU两次`oldThread=0` switch-in，要求质量计数恰好为1，并验证Query 1.34返回4条原始区间、其中缺口区间明确不完整。

#### 恢复与重复成本修复

- CPU/GPU Zone正式文件通过identity、size和SHA-256审计后可以复用。
- 复用后才删除`zones.work`/`extras.work`；删除失败会阻止继续，不能静默泄漏。
- 通用Session domain index在自身完成后立即原子发布。后续域失败重试时不再重复扫描全部Canonical。
- Welcome `timerMul/initBegin`建立独立、带强身份和SHA-256的Global Time Transform表；所有派生域共享，且完整扫描仍检查多个Welcome之间的冲突。
- Converter恢复过程仍先验证19 GiB source强身份与全部Canonical shard；原始stream只读且未被删除。

当前回归：

```text
tracy-trace-session-inventory-tests  Passed
```

当前converter：

```text
build-n30-capture\Release\tracy-stream-convert.exe
SHA-256 DA97785F30AB1FBA9FE44D48E55184FB461BE0BA5045B5B645CF9C0E08C92D44
```

真实30分钟Session正在从已校验Canonical恢复，尚不能在本节标记最终发布通过。

### 2026-08-31 真实30分钟规模阻断与Packed Canonical修复

真实输入：

```text
N29-Autonomous-30m\Run-20260830-220012\Admin-HighEvidence-30m.tracy-stream
source bytes:           19,122,822,908
protocol frames:             193,141
protocol events:       2,284,723,486
decoded protocol bytes: 53,791,117,815
```

首次实验Canonical在处理约98.5% source record时已占用约165.77 GiB、7,097个文件。根因不是内存泄漏，而是旧物理格式为每个逻辑事件重复写入56字节Canonical头；仅事件头理论值就约127.8 GiB。旧Mandatory Domain Index还计划为每个逻辑事件写48字节索引行，理论上会再增加约102.1 GiB。二者叠加会在其他语义索引之前突破256 GiB Session门禁。

修复：

- Canonical shard改为`PackedProtocolFrame encoding 2`：一个原始压缩journal record只写一份完整解码协议帧和一个固定物理头。
- 读取时继续调用唯一的`CountTraceProtocolFrame`解析器，惰性恢复每个逻辑事件的QueueType、domain、frame offset、thread context、semantic time和variable payload。
- `shard.recordCount`继续表示逻辑事件、Protocol Frame和transport record总数；物理记录数不冒充逻辑事实数。
- Canonical source order不再需要把一个checkpoint段的全部逻辑事件物化到内存；一个packed data shard按原始顺序流式展开。
- 通用`session-index`删除48字节逐事件重复表，改为每shard固定摘要。Frame、Job、CPU/GPU Zone、Memory、Sampling、Scheduling、Relation、Runtime、I/O/Gfx和GPU Analysis仍由各自的精确语义索引提供查询。
- Inventory容量估算改为`decoded protocol bytes + non-protocol payload + fixed physical record overhead`再加安全系数。本次数据的新Canonical基数约53.83 GB，默认120%估算约64.60 GB；加Derived和temporary后总预估约93.28 GB，低于256 GiB门禁。
- checkpoint内部物理编码升级为2；对外Session/Canonical schema仍保持1。旧实验building被保留，但自动恢复只读取最后checkpoint做有界兼容探测并快速排除，不会先哈希约166 GiB，也不会把新旧格式混写。
- 旧转换在已提交checkpoint处强制停止；原始stream和旧building均未删除、未改写。

TDD证据：

```text
RED:   packed canonical physical bytes scale with decoded frame bytes, not per-event headers
GREEN: tracy-trace-session-inventory passed (0.56 sec)
```

目标回归同时验证：

- 逻辑事件、Protocol Frame、transport record和encoded bytes与Inventory 100%一致。
- 跨域事件顺序、ThreadContext和semantic time不变。
- packed frame物理大小不再包含每事件56字节头。
- generic index bytes只按shard数量增长。
- 旧encoding 1 checkpoint明确返回`canonical_packed_encoding_unsupported`。
- 损坏checkpoint仍返回明确integrity错误。

全量重链后的八项回归：

```text
tracy-query-contract              Passed
tracy-gpu-analysis                Passed
tracy-trace-session-store         Passed
tracy-trace-session-inventory     Passed
tracy-query-mcp-transcript        Passed
tracy-query-version               Passed
tracy-query-doctor                Passed
tracy-gpu-analysis-n29-static     Passed
100% tests passed, 0 failed (1.84 sec)
```

Protocol 90 Catalog smoke：

```text
source bytes:       4,141,179
logical records:      202,642
canonical shards:           2（1 data + 1 checkpoint）
generic index bytes:       56
GPU resources:              1
Session state:       Complete
tracy-query doctor:  ready / schema 1.34.0
Session files/bytes: 44 / 25,174,213
```

构建身份：

| 工具 | SHA-256 |
|---|---|
| `build-n30-query\Release\tracy-query.exe` | `AFD0FEB173298BEBFDD6C76474864D7DA4B1C86BC4705BBDF7BDF319DEF72C5D` |
| `build-n30-capture\Release\tracy-stream-convert.exe` | `3A5BD27A452FBF4649E0EE93B311E934EDC5F8602049EC634F2E63FDED0D44A3` |

另一个34.0 MB旧诊断capture完成Inventory和Packed Canonical后，被现有CPU Zone语义门禁以`session_cpu_zone_end_before_begin`拒绝；该capture从已打开Zone中途开始，不能作为完整Session Oracle。拒绝行为证明Mandatory Derived不会为不完整源数据伪造Zone Begin。

真实30分钟compact generation和Mandatory Derived尚未在本小节宣称通过；必须使用上述新版converter重新执行，旧165.77 GiB generation不得作为验收结果。

### 2026-08-31 Windows 原子 Manifest 提交回归

真实30分钟输入首次进入Canonical后，在第12组Checkpoint之后失败：

```text
session_atomic_replace_failed:1175
ERROR_UNABLE_TO_REMOVE_REPLACED
```

现场保留了旧`manifest`、未发布的`manifest.tmp`以及194个已校验Canonical文件；没有覆盖原始stream，也没有把building目录发布为可查询Session。根因是Windows `ReplaceFileW`要求旧文件的临时读者共享删除，并且会尝试合并旧文件ACL。索引、杀毒或状态读取造成短暂占用时，原实现第一次失败便终止长转换。

修复：

- 对微软保证旧文件和replacement文件仍保持原名的`ERROR_UNABLE_TO_REMOVE_REPLACED`，以及Sharing/Lock/Access类可恢复错误，执行总计不超过1.5秒的指数退避重试。
- 使用`REPLACEFILE_IGNORE_ACL_ERRORS`；replacement由同目录创建，继承相同目录ACL，ACL合并失败不再破坏数据提交。
- 重试耗尽后保留旧manifest和`.tmp`，错误增加目标文件名，不删除、不降级、不猜测发布状态。
- 新增确定性Windows测试：以不共享删除的句柄持有manifest 75 ms，旧实现RED失败，修复后GREEN并验证新manifest内容。

回归：

```text
tracy-trace-session-store         Passed
完整CTest                         8/8 Passed
```

修复版对保留现场执行resume后已越过原故障点；Canonical文件由194继续增长，进程私有内存约0.48 GiB。真实30分钟最终发布与Query/MCP结果在本节后续验收中继续记录。

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
- CPU Zone域使用`cpu-zone-index/2/exact/cpu-zones.bin`：只在构建期保留各逻辑线程的活动Zone栈，闭合Zone立即写固定宽度磁盘记录；父子关系、child count、self time和开放捕获边界均由Canonical事实重建。
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
- Sampling域使用`sampling-index/2/exact/samples.bin`：构建时按native thread使用独立顺序work file，普通Sample与ContextSwitch Sample立即落盘；Hardware Sample使用有界chunk外部排序并按address/kind/source ordinal归并；内存不保存与总样本数成比例的事件容器。
- Sampling Reader严格保持Worker的输出顺序：native thread升序，每线程先普通Sample、再ContextSwitch Sample；时间复用`m_refTimeCtx`语义，Sample、ContextSwitch Sample、ContextSwitch和ThreadWakeup共同推进同一delta时钟。
- `CallstackPayload`与`CallstackSampleDictionary`进入统一内容寻址Callstack ID空间；普通Sample消费pending callstack，Ref Sample只接受已经声明的dictionary id，未知、重复或协议状态冲突均阻止构建。
- `sample.list`现由Session Sampling Reader直接分页扫描固定宽度记录，不实例化Worker；能力只声明已经实现的`sample.list`，Ghost Zone、Flamegraph和Symbol Statistics仍保持不可用，禁止过度宣称。
- 六类PMU Hardware Sample（cycles、retired、cache reference/miss、branch retired/miss）已写入同一Sampling索引；Session打开只物化按地址聚合的有界摘要，事件通过磁盘offset按address/kind分页读取。
- `samples.bin`保存source强身份、generation、普通/ContextSwitch Sample、dictionary与Callstack payload计数；Final Audit执行完整SHA-256并与Inventory四类Sample QueueType逐项守恒。
- Scheduling域新增`scheduling-index/1/exact/scheduling.bin`：每个native thread和CPU使用独立顺序work file，切入立即追加，切出只原位补齐End/Reason/State；构建期内存仅保留每线程/每CPU最后一个开放区间和pending wakeup。
- `ThreadWakeup`、`ContextSwitch`以及两类Sample共享Worker的`m_refTimeCtx` delta时钟；wakeup时间/CPU和实际run时间/CPU分别保存，跨CPU唤醒不会被误写成实际运行CPU。
- 线程区间严格恢复Wakeup→Start→End、wait reason/state和开放边界；CPU区间独立恢复实际占用线程，外部线程压缩索引按Worker首次切入顺序生成。
- `context_switch.range/thread/statistics`、`cpu.timeline`、`cpu.topology`、`cpu.usage`和`thread.*`直接读取Scheduling Reader，不实例化Worker；CPU usage按页读取固定宽度磁盘记录。
- Final Audit对`scheduling.bin`执行完整SHA-256，逐项核对源`ContextSwitch`与`ThreadWakeup` QueueType计数；派生线程/CPU区间计数不能替代源记录守恒。
- GPU Zone域新增`gpu-zone-index/1/exact/gpu-zones.bin`：按真实GPU Context保存固定宽度Zone记录；构建期只保留每个Context/Thread的活动Zone栈、尚未收到`GpuTime`的Query ID和少量Context状态，不物化全部GPU Zone。
- CPU侧Begin/End时间分别复用Worker的`m_refTimeThread`与`m_refTimeSerial`语义；GPU时间由异步`GpuTime`按`Context + QueryId`原位补齐，并保留Context的period、calibration、overflow和time-diff状态。
- 父子关系、child count、self time、SourceLocation、Callsite和Callstack均由Canonical事实重建；捕获结束仍缺少GPU timestamp的Zone保留为`complete=false`，不得伪造结束时间，也不因合法在途Query使整个Session失败。
- `zone.gpu.contexts/search/get/tree/statistics`由Session GPU Zone Reader直接提供；能力只声明已实现方法，Annotation尚未物化时返回明确`unavailable_reason`。
- `gpu-zones.bin`保存source SHA-256、source size、manifest generation、Context/Zone/Complete/Source及GPU时钟事件计数；Final Audit重算完整SHA-256并逐项核对全部GPU Begin/End/Time/Calibration/Sync/Context QueueType。
- FrameImage域新增`frame-image-index/1/exact`：`frame-images.bin`只保存固定宽度图片目录，`frame-images.bc1`顺序保存图片块；打开Session只驻留目录，Raw分页或PNG/MCP资源请求时才读取单张BC1并按需解码RGBA。
- FrameImage转换严格复用传统Worker的`FixOrder + RDO`处理语义；`FrameImageData`和`FrameImage`必须一一配对，尺寸、BC1字节数、重复Frame以及on-demand frame offset均做显式校验。
- 图片目录保存width、height、flip、raw frame index、数据offset/length；`frame_image.list/metadata/resource/raw`均从Session Reader提供，Frame基础Set可证明时同步返回`frame_ref`，不能证明时保持null。
- Metadata与BC1数据分别保存source强身份、generation、文件大小和SHA-256；Final Audit完整校验两个文件并核对源`FrameImageData/FrameImage`事件守恒。BC1总量不进入Session打开内存。
- SourceLocation继续复用CPU Zone索引中与传统Worker一致的静态/动态Native ID排序；Callsite以固定宽度记录附加到同一索引，保留thread、source、全局Callstack ID、domain、flags、provenance和unavailable reason。
- Source/Callstack/Symbol域新增`symbol-index/1/exact`：`symbols.bin`保存内容寻址Callstack payload、native/managed Frame、inline Frame、Symbol元数据和Address Mapping；`symbol-code.bin`顺序保存机器码，打开Session时不把机器码载入内存。
- `CallstackPayload`、`CallstackSampleDictionary`和`CallstackAllocPayload`使用同一个确定性内容字典，普通、Sampling、Job、Zone和Memory事件引用相同的全局Callstack ID；未知地址仍保留原始地址，不伪造符号。
- Native Frame的函数名、image和inline关系以`CallstackFrameSize/CallstackFrame`为权威；`SymbolInformation`只补充Symbol文件和行号，`SymbolCode`只补充机器码。Query不能把补充事件误解为新的函数名来源。
- `source.locations/source.callsite/search`、`callstack.resolve/frames/batch`、`symbol.search/get/address/address_map/raw_code`现由Session Reader直接提供；逐页机器码读取不会实例化完整Worker。
- Tracy多个协议域共享`SingleStringData/SecondStringData`暂存槽。CPU/GPU Zone构建器现在允许后续Callstack/Symbol/Message/Lock/GPU Context事件覆盖该槽，并只在实际消费者处读取，避免合法跨域字符串序列被误判为损坏。
- `symbols.bin`和`symbol-code.bin`分别保存source强身份、generation、文件大小和SHA-256；Final Audit逐项核对Callstack、Frame、Symbol和Code计数。任一文件篡改都会使强制派生层失效并阻止发布。

### TDD证据

RED：

- 尚无`GpuAnalysisStoreReader::OpenAt`时，Session GPU Reader测试按预期编译失败。
- 尚无`OpenSessionIfReady`和`TraceSourceKind::Session`时，SessionTraceSource测试按预期编译失败。
- Frame Canonical fact存在但磁盘语义Reader尚未实现时，测试按预期失败于`Session advertises Frame only after its disk-backed semantic reader is ready`。
- Job Canonical fact存在但磁盘语义Reader尚未接入时，测试按预期失败于`Session advertises Job only after its disk-backed semantic reader is ready`。
- Memory Canonical fact存在但磁盘语义Reader尚未接入时，测试按预期失败于`Session advertises Memory only after its disk-backed semantic reader is ready`。
- Sampling Canonical fact存在但磁盘语义Reader尚未接入时，测试按预期失败于`Session advertises Sampling only after its disk-backed semantic reader is ready`。
- Scheduling Canonical fact存在但磁盘语义Reader尚未接入时，测试按预期失败于`Session advertises Context Switch only after its disk-backed semantic reader is ready`。
- Runtime Domain和C#/Lua Script Canonical fact存在但磁盘语义Reader尚未接入时，测试按预期失败于对应Session能力尚不可查询。
- Source/Callstack/Symbol Reader尚未接入SessionTraceSource时，新增直接Reader断言后Query路径按预期因磁盘能力不可用失败；共享`SingleStringData`仍被CPU Zone当成一次性值时，合法Callstack/Symbol序列按预期暴露`session_cpu_zone_single_string_sequence_invalid`。

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
- Runtime GREEN恢复1个`ScriptStack`域状态、1个Managed Source Frame、1个完整Stack、1个Marker和1个完整Zone；Query 1.34的`runtime.domain.states`与`runtime.script.summary`无需完整Worker即可得到相同结果。
- 二进制Script Schema 2查询不再误触只用于Schema 1/GC兼容的Message扫描；GC查询仍保留原有语义，不把尚未完成的Message Reader伪装为可用。
- Final Audit在`runtime-script.bin`同尺寸内容被篡改后返回`session_runtime_file_sha256_mismatch`。
- GPU Zone RED按预期失败于`Session advertises GPU Zone only after its disk-backed semantic reader is ready`。
- GPU Zone GREEN恢复一个D3D12 Context和`Synthetic GPU Zone`：CPU/GPU区间均为`[36,40]ns`、begin query ID为7、self time为4ns，SourceLocation为`GpuFunction (Gpu.cpp:789)`；直接Reader、`zone.gpu.contexts`和`zone.gpu.search`结果一致。
- Final Audit在`gpu-zones.bin`同尺寸内容被篡改后返回`session_gpu_zone_file_sha256_mismatch`。
- FrameImage RED按预期失败于`Session advertises FrameImage only after its disk-backed semantic reader is ready`。
- FrameImage GREEN恢复1张4×4、flip=true、raw frame index=1的BC1图片；直接Reader验证8-byte Raw分页和64-byte RGBA按需解码，Query `frame_image.list/raw`返回相同元数据、offset和字节数。
- Final Audit在`frame-images.bc1`内容被篡改后返回`session_frame_image_data_sha256_mismatch`。
- Source GREEN恢复4个SourceLocation和3个SiteReuse Callsite；Callsite 77稳定关联Job SourceLocation、全局Callstack ID 2及`SiteReused` provenance。
- Callstack GREEN将地址`0x20202020`展开为`InlineJob (Inline.cpp:41)`与`JobRoot (Job.cpp:42)`两个inline层，并忠实保留无法解析的`0x30303030`帧。
- Symbol GREEN恢复`0x2000/0x2100`两个Symbol、地址映射、inline provenance以及`0x2100`的2-byte机器码；直接Reader与Query 1.34的Source、Callsite、Callstack、Symbol和RawCode结果一致。
- Final Audit在`symbols.bin`同尺寸内容被篡改后返回`session_symbol_metadata_sha256_mismatch`。
- Query、MCP transcript、Session、GPU Analysis和N29静态一致性共8项回归全部通过。

### 尚未完成，不能提前通过N30.6

- FrameImage与基础`Frames` FrameSet的raw index关联已接入；仍需在短Trace传统Worker差分中覆盖On-demand首次连接、pre-capture图片丢弃和初始Frame offset变体。
- GPU Zone schema 2已经增加4096条一块的immutable时间包络与256-bit Context Bloom，时间窗口和指定Context可跳过不可能命中的块；父节点查询仍需posting index，并且Annotation及serial/fiber边界仍需传统Worker差分，不能据此提前通过长Trace查询性能门禁。
- Scheduling schema 5已经为线程/CPU固定宽度记录增加4096条一块的immutable时间包络与256-bit identity Bloom；时间窗口、指定thread和指定CPU可跳过不可能命中的块。仍需在真实长Trace验证查询延迟，若Bloom误命中导致门禁不通过，再升级为压缩posting list。
- Sampling schema 3已为普通Sample增加4096条一块的immutable时间包络和Thread Bloom；Hardware Sample已按address/kind建立精确offset。仍需对真实长Trace查询延迟做门禁。Callstack frame、符号和SourceLocation已可导航，但Parent Callstack、embedded Source、Symbol反汇编和Sample Symbol Statistics尚未完成，相关能力不得提前宣称。
- Job schema 2 与 `job-pages/1` 已将 Schedule、Config、Dependency、Stage 及稳定 Job ID 写成有序磁盘区段；Reader 打开时只加载有界的 Type、Frame 和 Callsite 元数据，`Scan/Get` 通过 ID 页读取，不再物化全部 Job DTO。剩余项是反向 Dependency posting、Frame→Job 关联以及真实长 Trace 延迟/内存门禁。
- CPU Zone schema 2已经增加4096条一块的immutable时间包络与256-bit Thread Bloom，时间窗口和指定thread可跳过不可能命中的块；children查询仍线性扫描单个`cpu-zones.bin`，SourceLocation元数据仍驻内存。在N30.6完成前必须增加parent→children索引并验证长Trace查询延迟，不能据此提前通过查询性能门禁。
- Memory schema 2已经为每个Pool增加4096条一块的immutable生命周期时间包络；`memory.events(pool_ref=...)`、`memory.active_at_time`和Frame Snapshot可跳过不相交Pool block。Frame Snapshot仍会物化最终与目标帧相交的事件集合，必须在真实长Trace验证窗口基数和峰值内存。
- Memory allocation/free到CPU Zone的交叉关联尚未接到磁盘CPU Zone Reader；当前Callstack可导航，但`allocation_zone_ref/free_zone_ref`在Session路径仍为空，不得提前宣称跨域Memory证据链完整。
- Runtime/Script Reader打开时只保留文件偏移和计数，但当前脚本查询仍会物化所请求域的全部Frame/Stack事件；N30.6完成前必须增加分页/范围读取。Message Reader已完成，但Legacy GC消息编码与GC聚合语义仍需单独差分，`memory.gc.*`暂不得提前宣称Session可查。
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

### 2026-08-31 GPU Zone Reader 构建身份与回归

| 工具 | SHA-256 |
|---|---|
| `build-n30-query\Release\tracy-query.exe` | `DB3A3AB97032A8F266E3448CA9C154305D1A4CED0E2B5F20902B033C60251B6C` |
| `build-n30-capture\Release\tracy-stream-convert.exe` | `A9FE09427B37B1EDD2E94BFEFA6117ED558C480BA997C81EA76E94556CA03E64` |

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

### 2026-08-31 FrameImage Reader 构建身份与回归

| 工具 | SHA-256 |
|---|---|
| `build-n30-query\Release\tracy-query.exe` | `E7C29C096564D2072F87B58225905476371492E9A023C6FBB5AE2D4F9296C580` |
| `build-n30-capture\Release\tracy-stream-convert.exe` | `F1AC6E5D9257C3324D5D22C089A1E3688F1F779FCB525BF68BE836D7595BF01C` |

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

### 2026-08-31 Source/Callstack/Symbol Reader 构建身份与回归

| 工具 | SHA-256 |
|---|---|
| `build-n30-query\Release\tracy-query.exe` | `86B92CD1C9C789E5A3BBF9DB428354D7310663D31AA07305B81D34E9877B9C10` |
| `build-n30-capture\Release\tracy-stream-convert.exe` | `7DF9EEB900099B4B266DED5B964B973EF62400660669EA30D888808EB2464D8B` |

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

### 2026-08-31 Relation Reader

实现和正确性：

- 新增 `derived/relation-index/1/exact/relations.bin`，按Canonical source order流式写入固定宽度记录；Session打开只校验并加载有界元数据，不物化全量Relation。
- 每条记录保留转换后的精确时间、source/target ID、线程、Entity Kind、Namespace、Relation Kind和flags。
- `relation.search`与`relation.get`通过`ScanRelations(offset, limit)`分页读取；能力只有在不可变Relation索引成功打开后才标记`present/indexed/queryable`。
- Final Audit要求Canonical `JnRelation`计数、Relation索引计数和manifest计数完全一致；文件大小、source identity和SHA-256任一不符均拒绝使用。
- Synthetic Oracle验证`Job 500 → GPU Pass 900`、`Job/ExecutesPass`、转换时间34 ns；损坏最后一个record字节后返回`session_relation_file_sha256_mismatch`。

TDD证据：

```text
RED:   Session advertises Relation only after its disk-backed semantic reader is ready
GREEN: Trace Session Inventory tests passed
```

构建身份：

| 工具 | SHA-256 |
|---|---|
| `build-n30-query\Release\tracy-query.exe` | `08CC590771CB36AA40159C5E1C0C66A7810DFF0441209A1AA0FFE53235D4D90E` |
| `build-n30-capture\Release\tracy-stream-convert.exe` | `F50AC526C343ADDB710532256E700927ABB787D373E2CCC853C9970E2C373F4E` |

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

### 2026-08-31 Runtime Domain 与 C#/Lua Script Reader

实现和正确性：

- 新增`derived/runtime-index/1/exact/runtime-script.bin`，以固定宽度区域保存Runtime Domain State、Script Frame和Script Stack事件，字符串在同一不可变文件尾部去重保存。
- Session打开只验证identity、布局、文件大小和SHA-256，并保存各区域偏移；不创建完整Worker，也不在打开阶段物化脚本事件。
- `runtime.domain.states`恢复Synthetic `ScriptStack`域generation 3、requested frame 20及Enabled状态。
- `runtime.script.summary`恢复1个Managed Frame、1个Stack、1个Marker、1个完整Zone，并通过Relation索引恢复Script Zone到Frame的精确关联。
- 修复Schema 2二进制Script查询仍无条件扫描Legacy Message的问题。Script Schema 2不再加载Message；`memory.gc.*`仍保留旧消息语义，等待独立Message Reader后再对Session开放。
- Header布局计算使用乘法和加法溢出保护；文件内容篡改由Final Audit返回`session_runtime_file_sha256_mismatch`。
- 当前Reader仍以全量vector返回一次脚本查询所需的Frame/Stack事件，后续必须增加分页/范围读取，不能据此宣称长Trace查询内存门禁通过。

TDD证据：

```text
RED:   Session advertises Runtime Domain/Script only after its disk-backed semantic reader is ready
GREEN: Trace Session Inventory tests passed
```

构建身份：

| 工具 | SHA-256 |
|---|---|
| `build-n30-query\Release\tracy-query.exe` | `98E88FA6DB449C64DACFEE31558E1E6E40B9407715325852005CE581B6B40E4C` |
| `build-n30-capture\Release\tracy-stream-convert.exe` | `B56E0724ADCCED83C3699A23C2E681A5B2E5761729866010E70DF6D677FBB915` |

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

### 2026-08-31 I/O、Graphics Jobs 与关联 Frame Reader

实现和正确性：

- 新增`derived/io-gfx-index/1/exact/io-gfx.bin`，以单个不可变文件的七个固定宽度区域保存I/O Request/Config/Stage、Gfx Dispatch/Entity/Link和JN Correlated Frame事件。
- Converter生产路径使用七个有界`.work`流并在提交阶段拼接，不为全部I/O Request或Gfx实体建立随录制时长增长的内存表。
- Session打开校验source identity、区域布局、文件大小和SHA-256；只有校验成功后才开放`io`和`job.gfx`能力。
- Synthetic Oracle恢复I/O Request 400的Queue/Start/Complete精确时间36/38/40 ns、4096 bytes和Success终态；恢复Gfx Dispatch 300、Gfx Job实体301以及`Dispatches`关系，并保留Frame 9关联。
- Final Audit要求七类Canonical QueueType计数、磁盘索引计数和manifest计数完全一致；篡改文件最后一个字节后明确返回`session_io_gfx_file_sha256_mismatch`。
- `io.search`和`job.gfx.statistics`已通过Session TraceSource读取磁盘索引，不再依赖完整Worker。
- 当前Query调用仍会把所请求域的I/O Request或Gfx DTO物化为vector；后续需继续改成分页/范围扫描，不能据此宣称长Trace查询内存门禁已经通过。
- I/O producer quality依赖AppInfo配置；独立AppInfo/Message Reader完成前，Session可返回精确事件，但完整性状态仍保持保守判断。

TDD证据：

```text
RED:   Session advertises I/O/Gfx only after its disk-backed semantic reader is ready
GREEN: Trace Session Inventory tests passed
```

构建身份：

| 工具 | SHA-256 |
|---|---|
| `build-n30-query\Release\tracy-query.exe` | `D7A136FDF13A5B4A2EA7CE9BE8EC423C86F08A2E0D7A76DF13327B39CE9EDC06` |
| `build-n30-capture\Release\tracy-stream-convert.exe` | `0FB325D0754FB0751FC064DEC8442F4110D2057621595AC548F97799790D20BF` |

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

### 2026-09-01 Plot 磁盘语义索引（N30.6A 子阶段）

实现和正确性：

- 新增`derived/plot-index/1/exact/plots.bin`，保存Plot定义、名称、配置、聚合值和分页点数据；Session查询不再构建完整Worker。
- Builder单次顺序扫描Canonical，按Plot使用最多64个可复用临时流，最终原子发布不可变文件和SHA-256 manifest。
- Final Audit将`PlotDataInt/Float/Double`、`PlotConfig`和`PlotName`源事件计数与索引独立计数对账；任何文件大小、身份或SHA不一致均拒绝使用。
- `plot.list`、`plot.points`、`plot.range`、`plot.downsample`和`plot.statistics`已通过现有Query协议读取Session Plot Reader。
- 老的已发布N30 Session可能早于Plot索引，因此打开时保持兼容：Plot能力明确返回“该Session早于Plot语义索引”，不会加载完整Worker或伪造空数据；新Session发布前必须构建并审计Plot索引。

TDD证据：

```text
RED:   Session advertises Plot only after its disk-backed semantic reader is ready
RED:   孤立PlotName被错误物化为第二个Plot，2条Session/Query断言失败
GREEN: Trace Session Inventory tests passed
```

测试基础设施说明：

- 新增Plot断言后，MSVC Release `/LTCG`使历史单体Inventory测试超过Windows默认1 MiB栈，进程以`0xC00000FD`退出。
- 同一测试EXE仅把栈保留改为8 MiB后，全部断言在1.49秒内通过，证明不是递归或产品路径故障。
- 最终只对`tracy-trace-session-inventory-tests`测试目标设置`/STACK:8388608`；converter、query、profiler等产品二进制不受影响。
- 默认构建的最终测试在1.30秒内通过；本子阶段未启动真实30分钟转换，也未重建真实Session。
- 快速非LTCG开发缓存首次构建时，`TracyQueryService.cpp`超过普通COFF节数上限并稳定报`C1128`；根因是该单元原先只依靠正式构建的`/GL`规避限制。现仅对该源文件增加MSVC`/bigobj`，不改变产品优化或运行语义。
- Plot名称现在先进入待解析名称表；只有`PlotData*`或`PlotConfig`引用对应native name时才创建Plot，孤立`PlotName`不再产生虚假实体。
- 对照`Worker::InsertPlot`确认标准Worker按协议到达顺序追加点数据，并不按时间重新排序；Session保持相同顺序，因此不引入与Worker语义不一致的外部排序。
- 快速开发缓存的增量构建为4.31秒，修复后的完整Inventory测试为1.13秒。

尚未在本子阶段宣称通过：

- 全部八项回归与Query/Capture成套产品二进制重建。
- 真实30分钟Session的Plot回填与性能门禁。

### 2026-09-01 Message 磁盘语义索引（N30.6A 子阶段）

实现和正确性：

- 新增`derived/message-index/1/exact/messages.bin`，以固定宽度消息记录、直接字符串区和literal指针字典保存Message语义；Session打开只保留文件偏移与有界literal字典，不创建完整Worker。
- 覆盖普通、颜色、callstack、颜色+callstack、literal及literal+callstack协议变体；`MessageAppInfo`单独计数，不伪装成时间线消息。
- Native Callstack与Managed `CallstackAllocPayload`使用内容去重的稳定Session callstack ID；每线程下一条callstack消费语义与协议一致。
- 彩色literal使用`QueueMessageColorLiteral::text`的真实字段偏移；测试证明literal可以先被消息引用、后收到`StringData`定义，构建器仍从完整不可变字典精确解析。
- Final Audit将九类Message QueueType源计数与索引计数逐项对账；身份、大小或SHA-256不一致均拒绝能力开放。
- `message.search`已通过Session Reader查询精确文本、颜色、线程和callstack引用，不依赖完整Worker。

TDD证据：

```text
RED:   Session Message能力在没有磁盘语义Reader时不可查询
RED:   Native/Managed MessageCallstack缺失，直接断言失败
RED:   MessageLiteralColor错误读取普通literal字段，强制索引拒绝发布
GREEN: Trace Session Inventory tests passed
```

诊断与构建说明：

- 快速开发构建曾遗留5个没有`cl.exe/link.exe`子进程的MSBuild外壳；仅结束明确PID后恢复，未中止有效编译。
- 测试最初复用`0x7000`作为literal和GPU Memory Pool名称，触发全局字符串冲突；改用唯一synthetic指针，保留生产端冲突阻断语义。
- 测试失败消息改为在派生函数返回后拼接，消除C++实参求值顺序导致的空错误文本。
- 本子阶段增量编译8～13秒，完整Inventory synthetic约5秒；未启动真实30分钟转换。

尚未在本子阶段宣称通过：

- Legacy GC专用消息编码及`memory.gc.*`聚合的传统Worker差分。
- 全部八项回归、Query/Capture成套产品二进制重建和真实30分钟Session回填。

### 2026-09-01 Lock 磁盘语义索引（N30.6A 子阶段）

实现和正确性：

- 新增`derived/lock-index/1/exact/locks.bin`，保存Lock定义、线程表、固定宽度时间线记录和自定义名称；Session打开只物化锁定义及每锁最多64线程的有界元数据，事件按范围从磁盘读取。
- 恢复`LockAnnounce/Terminate/Name/Mark`及exclusive/shared Wait、Obtain、Release状态；保留lock count、owner、waiter集合、contended、source location和自定义名称。
- 共享serial clock会同步消费Memory与serial GPU Zone事件，Lock事件的时间换算与标准Worker保持相同基准。
- Core状态错误（未announce事件、重复announce/terminate、无wait obtain、无owner release、shared类型不符、超过64线程）阻止generation发布，不猜测修复。
- Final Audit逐项对账十类Lock QueueType；`locks.bin`身份、大小或SHA-256不一致时拒绝能力开放。
- `lock.list`与`lock.timeline`已通过Query 1.34直接读取Session磁盘索引；synthetic恢复Lock 700的`wait→obtain→release`、owner及lock count。

TDD证据：

```text
RED:   Session Lock capability为pending，GetLocks/ScanLockEvents不可用
GREEN: Session Lock direct reader、Query list/timeline和既有Frame/Memory Oracle同时通过
GREEN: Plot/Message/Lock committed文件同尺寸篡改均被SHA-256审计拒绝
```

构建稳定性：

- 当前终端下`/m:8`在编译完成后两次遗留5个无`cl/link`子进程的MSBuild node；改用`/m:1`后同一增量构建稳定在7～16秒完成，不再把孤儿node误判为有效编译。
- Inventory synthetic约5秒；本子阶段未启动真实30分钟转换。

尚未在本子阶段宣称通过：

- SharedLockable、LockMark和多锁交错的传统Worker逐项差分。
- 长Trace时间/Lock索引性能、全部八项回归、产品工具重建和真实30分钟Session回填。

### 2026-09-01 Hardware Sample 磁盘语义索引（N30.6A 子阶段）

实现结果：

- Sampling索引schema升级到2；六类PMU事件与普通Sampling共享一个强身份、SHA-256和Final Audit边界。
- Producer事件先顺序写入固定宽度work file，再以最多1,048,576条记录的有界chunk执行外部排序；最终按`address → kind → source ordinal`合并，不把总事件集合装入内存。
- `samples.bin`保存按地址摘要、六类事件count和精确磁盘offset；Session打开只物化地址摘要，`hardware_sample.events`按地址、kind、offset和limit读取所需区间。
- 时间语义与Worker一致：非零时间使用`(tsc-baseTime)*timerMul`，协议中的零时间保持0。
- Final Audit将六类QueueType源计数之和与索引事件数对账；文件身份、大小、header边界或SHA-256不一致均阻止能力开放。
- `hardware_sample.address/counts/events/capabilities`已由Session直接查询；trace counts返回事件总数，不以地址数冒充事件数。

TDD证据：

```text
RED:   Session advertises Hardware Sample only after its disk-backed semantic reader is ready
GREEN: Trace Session Inventory tests passed
```

Synthetic覆盖同一地址的cycles、retired、cache reference/miss、branch retired/miss各一条；直接Reader和Query 1.34均恢复六类count、事件顺序及16/18/0/20/22/24ns时间。现有Sampling SHA损坏测试同时覆盖新增区域。

仍待N30.6集中门禁：

- 使用大规模Hardware Sample corpus验证外部排序的峰值内存、run数量、查询延迟和取消恢复。
- 与传统Worker对相同短Trace逐address、kind、eventIndex和timeNs执行100%差分。

### 2026-09-01 Thread 与 CPU Topology 紧凑索引（N30.6A 子阶段）

实现结果：

- Scheduling索引schema升级到4，在ContextSwitch/CPU timeline之后追加按native ID排序的Thread摘要、字符串区和按logical CPU排序的Topology表。
- Thread摘要只随线程数量增长，保存PID、local/external名称、Fiber标记、Zone/Message/Sample/ContextSwitch计数、running time、running regions、migration和group hint；Session打开不扫描完整事件时间线。
- 名称回退保持Worker优先级：local name优先；local name仅为数字TID时，可使用非`???`且非`ntdll.dll`的external thread name；没有任何名称时明确返回`???`。
- ThreadName、TidToPid、ThreadGroupHint、ExternalName metadata/payload、FiberName/Enter/Leave均逐QueueType计数并进入Final Audit；漏读任何一类都会以`session_thread_source_count_mismatch`阻止发布。
- `cpu.topology`保存package/die/core/logical CPU，重复logical CPU定义阻止发布；die字段在Protocol 90中标记available。
- `thread.list/get/statistics/timeline/migration`和`cpu.topology`已开放给Session Query；`cpu.usage`尚未开放，不会误入Worker回退。
- 无法仅由当前紧凑摘要证明的kernel sample count保持`null`，不伪造为0。

TDD证据：

```text
RED:   Session CPU topology调用仍回退到不可用Worker路径（进程返回1）
GREEN: Trace Session Inventory tests passed
```

Synthetic恢复`Main Thread`、PID 1001、group hint 7、普通Sample、ContextSwitch运行时间和两个logical CPU；直接Reader与Query 1.34结果一致。

仍待N30.6集中门禁：

- External Name数字TID回退、Fiber进入/离开、空名称、重复/缺失元数据和传统Worker字段级差分。
- 长Trace线程/CPU分页索引性能以及kernel sample count的精确来源链。

### 2026-09-01 CPU Usage 磁盘派生（N30.6A 子阶段）

状态：**Passed（synthetic correctness）**。

实现：

- Scheduling schema 4保存捕获进程PID，并从已持久化的CPU运行区间构造Worker等价的`own/other`并发曲线。
- `own`只在该线程存在本地Zone/Sample证据，或其`TidToPid`明确等于非零捕获PID时成立；未知PID不会因`0==0`被误分类。
- CPU区间只生成固定宽度Begin/End transition；transition以1,048,576条为有界chunk执行外部排序，再以最多64个输入文件的多轮归并按相同时间聚合，构建内存和同时打开的文件句柄均不随总ContextSwitch数线性增长。
- 输出包含Worker兼容的`time=0, own=0, other=0`基线点；仅当计数变化时追加记录。
- `cpu.usage`通过`ScanCpuUsage(offset, limit)`直接分页读取，不再由Query先物化全量曲线。
- Windows清理前显式关闭全部外部排序run句柄，避免成功发布后残留`cpu-usage-run-*.work`。

TDD证据：

```text
RED:   residual=cpu-usage-run-0.work
FIX:   close all run readers before deleting committed build temporaries
GREEN: Trace Session Inventory tests passed
```

Synthetic同时包含本进程thread 42与未知外部thread 43/100/101，验证基线点、own/other非零状态、统计计数、Query分页游标及发布前无`.work`残留。

### 2026-09-01 Scheduling 时间与实体块索引（N30.6B 子阶段）

状态：**Passed（synthetic correctness，长Trace延迟待集中门禁）**。

- 每4096条Thread/CPU运行记录生成一个immutable block，保存first/count、时间包络和256-bit identity Bloom。
- Bloom只用于排除不可能包含目标thread/CPU的块；命中后仍逐条核对固定宽度事实，因此允许假阳性但绝无假阴性。
- `context_switch.thread`、`thread.timeline/migration`和带`cpu`参数的`cpu.timeline`已路由到专用Reader；不再先扫描全域再由Query过滤。
- block表约按事件数的`1/4096`增长；不会复制逐事件posting list。若真实30分钟Trace的Bloom误命中率使查询超过5秒门禁，再增加压缩posting list，不能以牺牲正确性换速度。

Synthetic验证窄时间窗口、thread 42、CPU 0的直接Reader与Query结果，完整回归为：

```text
Trace Session Inventory tests passed
```

### 2026-09-01 Sampling 时间与线程块索引（N30.6B 子阶段）

状态：**Passed（synthetic correctness，长Trace延迟待集中门禁）**。

- Sampling schema升级到3，普通与ContextSwitch Sample保持Worker的thread/kind顺序，并追加4096条一块的时间包络和256-bit Thread Bloom。
- `sample.list`以及指定Thread的flamegraph扫描直接调用`ScanSampleEventsForThread`；Bloom命中后仍逐条核对Thread与时间，假阳性不会影响正确性。
- Hardware Sample保留address/kind精确offset，外部排序最终归并限制为最多64个同时打开的run；Windows删除前显式关闭全部reader。
- 索引根目录使用实际schema版本，不再把Schema 2/3数据错误写入`sampling-index/1`。
- 发布前测试枚举Sampling目录，任何残留`.work`都使阶段失败。

### 2026-09-01 CPU Zone 时间与线程块索引（N30.6B 子阶段）

- CPU Zone schema升级到2，在固定宽度Zone记录后追加4096条一块的immutable block目录。
- 每个block保存精确记录范围、时间包络和256-bit Thread Bloom；Bloom只用于排除不可能命中的block，Reader仍逐条校验thread和时间交集，因此误命中只增加I/O，不会改变结果或丢失Zone。
- `zone.cpu.search`在提供`thread_ref`时直接调用磁盘Reader的`ScanThread`，cursor中的raw offset以线程匹配子集为准，分页不会重复或遗漏。
- inverted source clock仍忠实保留两个源端点并沿用既有交集语义；测试中的`[32,-170]ns`异常Zone会与`[15,21]ns`窗口相交，不能为了索引性能隐藏该源事实。
- 索引根目录使用实际schema版本；header、manifest、Derived Audit同时核对block数量、布局、覆盖范围、文件大小和SHA-256。
- 单节点inventory测试与六项N29/N30组合回归通过（6/6）。

仍待N30.6集中门禁：真实长Trace时间/thread查询延迟和SourceLocation驻留内存评估。

### 2026-09-01 GPU Zone 时间与 Context 块索引（N30.6B 子阶段）

- GPU Zone schema升级到2，在固定宽度Zone记录后追加4096条一块的immutable block目录。
- 每个block保存记录范围、GPU/CPU fallback时间包络和256-bit Context Bloom；Reader仍逐条核对Context及原有GPU Zone时间交集，Bloom误命中不会改变结果。
- `zone.gpu.search`新增可选`context_ref`下推，Session路径直接调用`ScanContext`；Legacy Worker路径保持正确的兼容过滤。
- Context 0是合法身份，解析与Bloom不能把它误判为缺失；synthetic已验证`gpu-context:0`和`[37,39]ns`精确返回唯一Zone。
- header、manifest与Final Audit同时核对block数量、连续覆盖、布局、文件大小和SHA-256；索引根目录跟随schema版本。
- 单节点inventory测试与六项N29/N30组合回归通过（6/6）。

仍待N30.6集中门禁：Annotation、serial/fiber传统Worker差分与真实长Trace延迟。

### 2026-09-01 Memory 时间与 Pool 块索引（N30.6B 子阶段）

- Memory schema升级到2；每个Pool的固定宽度allocation生命周期记录按4096条生成immutable block目录。
- block保存最早allocation时间与最晚free时间；含未释放allocation的block保持开放上界，不能为了加速错误排除仍存活事件。
- `memory.events`的`pool_ref`下推到`ScanPool`，`memory.active_at_time`将目标时刻作为点窗口下推，之后仍按原语义精确检查`allocation<=time && free>time`。
- Frame Snapshot按选定Pool和生命周期block跳过不相交磁盘区段；只物化最终相交事件，未改变owned/active统计算法。
- header、Pool表、block连续覆盖、manifest和Final Audit核对event/block总数、文件大小、身份与SHA-256；索引根目录跟随schema版本。
- synthetic验证两个Pool、四条生命周期、pointer reuse、开放allocation、allocation/free callstack、Pool过滤和active-at-time；单节点inventory与六项组合回归通过（6/6）。

仍待N30.6集中门禁：allocation/free→CPU Zone交叉关联、真实长Trace时间/Pool查询延迟及高基数Frame Snapshot峰值内存。

Synthetic验证thread 42的普通/ContextSwitch Sample、Query thread filter、Hardware六类PMU事件、block计数和临时文件清理：

```text
Trace Session Inventory tests passed
```

### 2026-09-01 CPU/GPU Zone 父子 Posting（N30.6B 子阶段）

状态：**Passed（synthetic correctness，长Trace延迟待集中门禁）**。

- CPU/GPU Zone schema统一升级到3；固定宽度Zone与时间块之后追加按`(parentId, childId)`排序的精确posting区段。
- 构建阶段从既有Zone工作流顺序生成16字节Pair；每4,194,304条（64 MiB）形成一个有序run，最多64路归并，多轮合并时内存和同时打开的文件句柄均有界。
- Posting按parent、child稳定排序；`zone.cpu.tree`和`zone.gpu.tree`通过两次磁盘二分查找定位父节点区间，只读取请求页的child ID和对应Zone，不再线性扫描整个Zone表。
- 排序不存在采样、Bloom或近似；Reader读取后仍校验每个child的真实`parent`字段，索引与事实不一致时拒绝返回伪造结果。
- header、manifest、最终文件布局、child数量、SHA-256和临时run清理由既有Final Audit链保护；Schema 2不会被Schema 3 Reader误读。
- Job Reader复核确认已使用`job-pages/1`磁盘分页；验收文档中“打开时物化全部Job DTO”的旧描述已纠正，未重复实现已有能力。

TDD与回归证据：

```text
CPU non-empty parent: child_links=1，zone.cpu.tree返回唯一真实child
GPU empty parent:     child_links=0，tree返回空集合而不是错误记录
Trace Session Inventory tests passed
固定N29/N30组合回归：6/6 passed
```

下一步继续收敛N30.6B剩余线性查询；本子阶段未启动真实30分钟转换。

### 2026-09-01 Job 反向依赖与 Frame Posting（N30.6B 子阶段）

状态：**Passed（synthetic correctness，长Trace延迟待集中门禁）**。

- 保留Job index schema 2；`job-pages` schema升级到2，并新增独立`job-postings.bin` schema 1。
- Posting保存两种精确映射：`prerequisiteJobId→dependentJobId`与`originFrameId→jobId`。二者来自Canonical Job Dependency、Config和Canonical Frame Begin事实，不使用时间接近猜测。
- Posting使用16字节固定Pair和有界外部排序；Page manifest同时保存posting文件大小、SHA-256和两类关系数量，Final Audit在Session发布前核对身份、布局与checksum。
- `job.dependencies`的Session路径不再调用`GetJobs()`；上游只按Job ID读取直接前置Job，下游通过reverse posting分页读取。
- `GetJobsForFrame`直接通过Frame posting分页；`GetEvidenceJobs`先读取该Frame直接Job，再按每个Job自己的前置依赖逐点扩展，不物化全Session。
- Legacy Worker继续使用原始全量实现，API语义不变；Job Critical Path和旧Correlation全图仍是后续分页改造项。

TDD证据：

```text
Synthetic: Job 400 → Job 500，Job 500 originFrameId=9
Reader:    Dependents(400)=[500]，FrameJobs(9)=[500]
Evidence:  Frame 9=[400,500]
Query:     job.dependencies(Job 400).downstream=[Job 500]
RED:       旧测试使用非协议字段query做文本筛选，被第二个Job暴露
FIX:       改用正式filter.text；未放宽结果断言
GREEN:     Trace Session Inventory tests passed；固定组合回归6/6
```

发布目录不残留任何Job `.work` 文件。本子阶段未执行真实30分钟转换。

### 2026-09-01 FrameIdentity 与 GfxDispatch Frame Posting（N30.6B 子阶段）

状态：**Passed（synthetic correctness，长Trace延迟待集中门禁）**。

- I/O/Gfx index schema从1升级到2，在既有固定宽度事实之后追加两类精确posting：`frameId→CorrelatedFrameEvent ordinal`与`frameIndex→GfxDispatch ordinal`。
- Producer顺序写入16字节Pair工作文件，再使用公共64 MiB有界外部排序器生成immutable posting；构建内存不随Frame事件或Dispatch总量线性增长。
- Session Reader新增按Frame分页读取接口；两次磁盘二分查找定位Frame区间，只读取请求页的posting和目标固定宽度记录。
- `frame.identity(ref/frame_id)`在Session中只读取指定Frame事件；`timeline.correlated_slice`只读取指定Frame事件、Frame→Job posting及Frame→Dispatch posting，不再调用全量`GetJobs()`、`GetGfxDispatches()`或`GetCorrelatedFrameEvents()`。
- `entity.related`和`correlation.chain`仍保留旧完整图语义，尚未错误宣称为长Trace分页实现。
- Final Audit除强身份、文件大小、布局和SHA-256外，还流式验证posting排序、目标ordinal范围及posting FrameId与目标事实一致；不能以“错误内容自己的正确checksum”通过发布。
- `frame.identity`与`timeline.correlated_slice`仅在correlated Frame事实和I/O/Gfx磁盘Reader均可用时开放Session capability；索引缺失不会回退到完整Worker。

TDD与系统化诊断证据：

```text
RED:       timeline.correlated_slice 返回 CAPABILITY_UNAVAILABLE
ROOT:      新Reader已实现，但Session capability方法表未声明该方法，Query在到达Reader前拒绝请求
FIX:       增加精确correlation capability；不放宽全局Session门禁
Reader:    Frame 9 dispatch=[300]，event count=1；Frame 10均为空
Query:     Frame 9 direct job=[500]，dispatch=[300]
GREEN:     Trace Session Inventory tests passed；固定组合回归6/6
```

发布目录不残留I/O/Gfx external-sort `.work` 文件。本子阶段未启动真实30分钟转换。

### 2026-09-01 Gfx 实体与关系邻接 Posting（N30.6B 子阶段）

状态：**Passed（synthetic correctness，长Trace延迟待集中门禁）**。

- I/O/Gfx index schema从2升级到3，新增`entityId→Entity ordinal`、`parentId→Child Entity ordinal`、`link sourceId→Link ordinal`和`link targetId→Link ordinal`四类精确posting。
- 四类posting与Frame posting共用64 MiB chunk、最多64路的有界外部排序器；不会把全部Entity或Link装入构建内存。
- Session `GetEvidenceGfx(frameId, seedIds)`先从Frame→Dispatch posting和seed建立工作集，再通过Parent与Link Source posting做按需BFS；`BelongsToFrame`反向入口通过Link Target posting解析。
- 返回的Entity和Link仍从固定宽度事实记录读取；posting只决定候选ordinal，Reader不把邻接ID伪装为实体事实。
- `evidence.graph`自动使用新的Session override；`timeline.correlated_slice`也改用同一Gfx局部证据路径，不再读取全Session Entity/Link。
- Final Audit逐条验证四类posting排序、ordinal范围和Entity/Link真实字段；identity、layout、SHA-256或语义键不一致均阻止Session发布。

Synthetic证据：

```text
Frame 9 seed Jobs: 400,500
Frame Dispatch:     300
Reachable Entity:   301 (parent=300)
Reachable Link:     300 -> 301 (Dispatches)
Excluded:           unrelated ExplicitGpuPass -> Resource link
Trace Session Inventory tests passed
```

本子阶段未改动Legacy Worker语义，也未开放尚未完成的全局`correlation.chain`分页能力。

### 2026-09-01 Job 指定根关键路径点查（N30.6B 子阶段）

状态：**Passed（synthetic correctness，长Trace延迟待集中门禁）**。

- Session 的 `job.critical_path(ref=Job)` 不再调用 `GetJobs()` 或读取完整 `jobs.bin`；从根Job开始，使用 `job-pages` 的ID点查递归构造精确上游依赖闭包。
- 闭包仍执行原有DAG最长执行时间算法；未改变零执行时长前置Job不进入耗时关键路径的既有规则。
- 返回增加 `scope=upstream_closure`、`scope_jobs`、`root_job_id`、`complete` 和 `missing_prerequisite_job_ids`，明确区分“根Job上游闭包”与无ref的全局关键路径。
- Capture边界导致前置Job缺失时不伪造完整结果，返回 `complete=false` 和 `dependency_closure_incomplete`；查询预算不足则返回明确 `RESOURCE_LIMIT`，不输出截断后冒充Exact的路径。
- 无ref的全局 `job.critical_path` 仍保持原有全Job语义，后续由外部拓扑归并实现长Trace磁盘化，当前不做近似。

TDD证据：

```text
RED:       隐藏 jobs.bin 后，旧 Session job.critical_path(ref=Job 500) 失败
FIX:       通过 GetJob(500) → GetJob(400) 磁盘点查构造上游闭包
RESULT:    processed_jobs=2，scope_jobs=2，关键耗时路径=[Job 500]
GREEN:     Trace Session Inventory tests passed；固定组合回归6/6
```

测试结束已移除临时Query响应诊断输出；本子阶段未启动真实30分钟转换。

### 2026-09-01 I/O Request ID Posting 与点查（N30.6B 子阶段）

状态：**Passed（synthetic correctness，I/O Search/Statistics/Chain分页仍待后续）**。

- I/O/Gfx index schema从3升级到4；新增统一的`requestId→(record kind, ordinal)`精确posting，覆盖Request、Config和Stage三类Canonical事实。
- record kind编码在posting value高2位，低62位保存对应固定宽度事实的ordinal；三类ordinal达到编码上限或总posting数量溢出时构建明确失败，不允许截断。
- posting使用既有64 MiB有界外部排序器；构建完成后顺序计算唯一Request ID数量，`request_count`不再依赖查询期物化全量DTO。
- Session Reader通过两次磁盘二分查找定位一个Request的全部事实，再点读Request/Config/Stage并复用与全量Reader相同的状态归并函数。
- `io.get`的Session路径现在只读取目标Request；Legacy Worker路径保持原有实现。`io.search`、`io.statistics`和`io.chain`仍保留全量路径，未错误宣称完成。
- Final Audit流式验证posting排序、kind、ordinal边界、目标Request ID和唯一ID总数；正确checksum无法掩盖语义键错误。

TDD与回归证据：

```text
Reader:    GetIoRequestCount()=1；GetIoRequest(400)恢复2个Stage；401不存在
Query:     io.get(400)返回requested/transferred bytes及完整Stage
GREEN:     Trace Session Inventory tests passed；固定组合回归6/6
```

发布目录不残留`io-request-id-posting`外排工作文件。本子阶段未启动真实30分钟转换。

### 2026-09-01 I/O Parent/Child Chain Posting（N30.6B 子阶段）

状态：**Passed（synthetic correctness，I/O Search/Statistics分页仍待后续）**。

- I/O/Gfx index schema从4升级到5；对`parentKind=IoRequest`且`parentId!=0`的Config事实新增`parentRequestId→child Config ordinal`精确posting。
- Session Reader通过父ID磁盘二分查找、Config点读和Request ID posting复原直接子Request；重复Config不会重复返回同一子Request。
- `io.chain`的Session路径从根Request开始，父方向用目标Request的Config点查，子方向用Parent posting，按需双向BFS；不再调用`GetIoRequests()`构建全Session邻接图。
- BFS受`max_nodes`和Query CPU预算约束；达到预算返回明确`truncated=true`，未访问节点不会伪装为完整链。
- 默认Worker实现继续使用原有全量扫描；分析层使用固定wire值`IoRequestParentKindValue=1`，避免为了默认兼容实现把协议Queue头耦合进所有TraceSource编译单元。
- Final Audit验证Parent posting排序、Config ordinal边界和真实`parentKind/parentId`；错误父子键会阻止Session发布。

TDD与回归证据：

```text
Synthetic: Request 400 → child Request 401，二者均为完整Read生命周期
Reader:    GetIoChildren(400)=[401]
Query:     io.chain(401)返回2 nodes、1 exact parent edge、truncated=false
RED/FIX:   首次编译暴露TraceSource头直接依赖协议枚举；改用分析层固定wire常量
GREEN:     Trace Session Inventory tests passed；固定组合回归6/6
```

新增synthetic事件把开放Frame尾部从40 ns推进到46 ns；Frame Reader与Query期望同步更新，未改变尾部闭合算法。本子阶段未启动真实30分钟转换。

### 2026-09-01 I/O Request ID 页表与 Statistics 分页（N30.6B 子阶段）

状态：**Passed（DTO物化有界；精确分位数外排仍待集中优化）**。

- I/O/Gfx index schema从5升级到6；从已排序的Request ID posting顺序生成唯一、严格递增的`io-request-ids`页表。
- 页表由posting事实确定性派生，不在内存保存全部ID；生成过程顺序读取、顺序写入，最终文件追加到immutable index并纳入SHA-256。
- Session Reader新增`ScanIoRequests(offset, limit)`，只读取请求页的ID，再通过ID posting点读各自Request/Config/Stage。
- `io.statistics`的Session路径以1024 Request为一页遍历，不再先调用`GetIoRequests()`生成全量DTO和全量Stage向量；Parent完整性通过目标ID点查验证。
- 当前延迟`queue/execution/total`的精确P50/P95/P99仍收集三个标量数组，内存与Request数线性但不再与Stage/DTO体量线性；后续使用外部排序或确定性磁盘run替换，不能用近似分位数冒充Exact。
- Final Audit线性对照唯一ID页表与ID posting的distinct key序列；缺失、重复、错序或键不一致均阻止发布。

TDD与回归证据：

```text
Reader:    ScanIoRequests(offset=1, limit=1)=[Request 401]
Query:     io.statistics requests=2，completed=2，unresolved_parent=0
BUILD FIX: vector声明触发most-vexing-parse，改为显式resize；未改变算法
GREEN:     Trace Session Inventory tests passed；固定组合回归6/6
```

本子阶段未把按Request ID排序错误用于`io.search`；其queue-time全局顺序仍待专用posting。

### 2026-09-01 I/O Queue-Time Posting 与 Search 分页（N30.6B 子阶段）

状态：**Passed（synthetic correctness，长Trace延迟待集中门禁）**。

- I/O/Gfx index schema从6升级到7；新增按`(queueNs, requestId)`严格排序的精确posting，以及按`requestId`排序的反向queue-time posting。
- 有符号queue time通过翻转符号位编码为无符号排序键，负时间、零时间与正时间的顺序和原始`int64_t`语义一致；没有Request事实的orphan保持既有`queueNs=0`语义。
- 同一Request出现多个Request事实时，构建器选择ID posting中ordinal最大的最后一个事实，与全量Reader逐事件覆盖后的结果一致。
- 两类posting使用既有64 MiB有界外部排序器；Session Reader只读取请求页的queue posting，再通过Request ID posting点查目标Request。
- Session `io.search`使用`ScanFiltered`和raw-offset游标按queue顺序增量扫描；文本、operation、source和status过滤不会触发`GetIoRequests()`全量物化。Legacy Worker路径维持原有排序语义。
- Final Audit逐项验证queue posting严格顺序、ID posting与唯一ID页表、由Canonical Request事实重算的queue time，以及两个排序投影的精确双向存在关系；任意错序、错时、缺失或多余记录均阻止Session发布。

TDD与回归证据：

```text
Reader:    ScanIoRequestsByQueue(offset=1, limit=1)=[Request 401]
Query:     io.search(limit=1)=[Request 400]，返回可继续的next_cursor
BUILD:     单节点Release目标通过
GREEN:     Trace Session Inventory tests passed；固定组合回归6/6
```

本子阶段未启动真实30分钟转换；`io.statistics`的精确分位数外排仍是独立后续任务。

### 2026-09-01 I/O 精确延迟外排统计（N30.6B 子阶段）

状态：**Passed（synthetic correctness，长Trace延迟待集中门禁）**。

- I/O/Gfx index schema从7升级到8；构建期按Request ID流式归并Request/Stage事实，只保留当前Request状态。
- Queue、Execution和Total三类有效延迟分别写入固定16字节Pair，经64 MiB有界外部排序形成immutable精确序列；不会在Query内保留随Request数量增长的三条`int64_t`数组。
- Session Reader直接从排序序列计算count、total、min/max、mean、P50/P90/P95/P99、stddev及P90截断均值；公式与既有`ComputeStatistics`保持一致。
- Session `io.statistics`仍以1024 Request分页计算生命周期与质量计数，但延迟统计改读磁盘精确摘要；Legacy Worker继续使用原有内存统计路径。
- Final Audit由Canonical Request/Stage事实重算每个有效延迟，验证三类posting严格排序、成员存在和总数；错误延迟即使文件checksum正确也不能发布。

Synthetic证据：

```text
Request 400: queue=2 ns，execution=2 ns，total=4 ns
Request 401: queue=2 ns，execution=2 ns，total=4 ns
Reader totals: 4 / 4 / 8 ns，三类count均为2
Query io.statistics与Reader一致
Trace Session Inventory tests passed；固定组合回归6/6
```

本子阶段未启动真实30分钟转换；Job statistics的六组精确延迟数组与诊断中的嵌套全表扫描仍待后续独立收敛。

### 2026-09-01 Job Handle 诊断 Posting（N30.6B 子阶段）

状态：**Passed（synthetic correctness，Job 精确延迟外排仍待后续）**。

- Job page schema从2升级到3，posting schema从1升级到2；新增`packedHandle→JobId`和`handle slot→JobId`两组精确磁盘posting。
- 两组posting直接从Schedule事实生成，并使用既有有界外部排序器；不在Session Reader中维护随Job数量增长的handle map。
- `TraceSessionJobReader`新增exact handle点查与按目标JobId邻近slot点查；后者只读取目标slot中围绕目标ID的有界窗口，再按距离稳定排序。
- Session `job.statistics`的缺失Ready/Queue诊断不再为每个样例调用`forEachStatisticsJob`扫描全部Job；每个样例最多点读9个exact-handle候选和9个near-slot候选。
- Legacy Worker保持原有结果语义；默认TraceSource实现仍可全量扫描，但Session路径使用磁盘posting。
- Final Audit除验证posting header、数量、offset、文件长度和SHA-256外，还逐项验证排序及其对应Schedule事实中的真实`packedHandle`/slot；错误语义键即使checksum正确也不能发布。

Synthetic证据：

```text
Job 600: packedHandle=0x100000ABC，缺失Ready
Job 500: packedHandle=0x000000ABC，Ready完整
Reader exact-handle(0xABC)=[500]
Reader near-slot(0xABC, 600)=[600,500]
Query missing_ready_examples[0].same_slot_jobs=[500]
Trace Session Inventory tests passed；固定组合回归6/6
```

本子阶段未启动真实30分钟转换；Job statistics的六组延迟数组仍按独立阶段处理。

### 2026-09-01 Job 精确延迟外排统计（N30.6B 子阶段）

状态：**Passed（synthetic correctness，真实长Trace墙钟与容量待N30.7集中门禁）**。

- Job page schema从3升级到4，posting schema从2升级到3；新增Schedule→Ready、Ready→Queue、Queue→FirstRun、Dependency Ready、Execution和Wait六类精确延迟序列。
- Page构建期以最多4096个Job为一页从磁盘复原完整Job语义，每页立即生成固定16字节`(signed latency, JobId)`事实；六类事实分别通过既有有界外部排序器生成immutable序列。
- Query `job.statistics`仍以1024 Job分页计算状态、质量、lane和诊断计数，但Session路径不再向六个随Job数量增长的`int64_t`数组追加数据。
- Session Reader直接从排序序列计算count、total、min/max、mean、median、P50/P90/P95/P99、stddev和P90截断均值；公式与Legacy `ComputeStatistics`一致，没有使用近似分位数。
- I/O与Job共用`ReadTraceSessionExactStatistics`，避免两套精确统计公式漂移；I/O既有数值与测试保持不变。
- Final Audit验证六类序列的header/count/offset、稳定有序性、Job ID存在性、文件长度和SHA-256；构建期测试另行逐值验证synthetic语义。

Synthetic证据：

```text
Job 500: Schedule→Ready=4 ns，Dependency Ready=8 ns，Execution=8 ns
Reader: schedule count=1/total=4；execution count=1/total=8
Query job.statistics与Reader的count/total一致
Trace Session Inventory tests passed；固定组合回归6/6
```

本子阶段未启动真实30分钟转换，也未修改录制协议、Unity或Player。

### 2026-09-01 GPU Pass→Gfx Link 邻接分页（N30.6B 子阶段）

状态：**Passed（synthetic correctness，真实长Trace延迟待N30.7）**。

- `TraceSource`新增`ScanGfxLinksFrom(sourceId, offset, limit)`；Legacy Worker默认实现保持原有过滤语义。
- Session Reader直接使用schema 9已有的Link Source posting定位目标Source ID，只读取请求页，不扫描或物化全部Gfx Link。
- `gpu.pass.resources`和`gpu.memory.by_pass`解析Explicit Pass→Reference Pass关系时，改为每页最多256条的目标Pass出边扫描。
- 每一页接入Query scan/CPU预算；预算不足返回`RESOURCE_LIMIT`，不会把部分Evidence Pass集合伪装成完整结果。
- 本子阶段没有改变GPU Catalog、ResourceSetV2、Range或显存核算语义，只缩小跨域关系读取范围。

TDD与回归证据：

```text
RED:       测试先调用不存在的ScanGfxLinksFrom，编译按预期失败
Reader:    ExplicitGpuPass → ReferencePass 1000仅返回1条ReferencesResources关系
GREEN:     Trace Session Inventory tests passed；固定组合回归6/6
```

本子阶段未启动真实30分钟转换，也未修改录制协议、Unity或Player。

### 2026-09-01 Runtime/Script 原始记录分页（N30.6B 子阶段）

状态：**Passed（raw-record bounded read；Script语义结果磁盘化仍待后续）**。

- `TraceSource`新增Runtime Domain State、Script Frame和Script Stack Event的精确count/page接口；Legacy Worker默认实现保持兼容。
- Session Runtime Reader直接按固定记录大小定位目标页，Frame和Stack文本仍从已校验的字符串区读取；分页不会先构造整个原始记录vector。
- `ParseN11Trace`使用4096条一页扫描Script Frame/Stack原始事实，并接入Query scan/CPU预算；不再调用Session的全量`GetScriptFrames()/GetScriptStackEvents()`。
- 事件顺序、Stack Header/Frame配对、Marker、Zone Begin/End、invalid/unresolved/orphan质量语义保持原实现。
- 本子阶段只消除了原始二进制事件的第一层全量物化。当前`N11TraceData`仍会为一次查询累积全部解析后的Frame、Stack、Marker和Zone JSON；长Trace下要满足最终内存门禁，仍需把这些语义对象建立为磁盘Derived并按查询页读取，不能把本子阶段标记为Runtime Script完全完成。

TDD与回归证据：

```text
RED:       测试先调用不存在的Runtime/Script count/page API，编译按预期失败
Reader:    Domain State=1；Script Frame=1；Script Stack Event=5
Page:      Frame(0,1)和Stack Event(1,2)返回精确原始记录
GREEN:     Trace Session Inventory tests passed
```

本子阶段未启动真实30分钟转换，也未修改录制协议、Unity或Player。

### 2026-09-01 Gfx Statistics 分页与点查（N30.6B 子阶段）

状态：**Passed（synthetic correctness，真实长Trace随机读取墙钟待N30.7集中门禁）**。

- `TraceSource`新增Gfx Dispatch、Entity和Link的精确count/page接口，以及Dispatch/Entity ID点查接口；Legacy Worker默认实现保持原有全量数据语义。
- Session Reader直接从I/O/Gfx immutable数据页读取最多1024条记录；Dispatch和Entity点查使用N30.6B既有精确posting，不物化完整Gfx集合。
- Session `job.gfx.statistics`在任何`GetJobs()/GetGfxDispatches()/GetGfxEntities()/GetGfxLinks()`全量读取之前进入专用路径。
- 统计只保留entity kind、link relation、完整性计数和少量样例；Parent、Link端点及关联Job通过精确点查验证，不维护随Gfx事件总量增长的DTO vector。
- 全域精确统计必须扫描全部Gfx事实。每页通过Query scan/CPU预算门禁；预算无法覆盖整个输入时返回`RESOURCE_LIMIT`，不得返回局部统计并伪装完整结果。
- 本次没有升级I/O/Gfx schema：实现复用schema 9已经提供的Dispatch、Entity、Parent及Link双向posting。
- 点查在大规模真实数据上可能产生较多随机I/O；N30.7必须验证跨域查询`≤5秒`。若失败，应把相同的精确标量摘要移到Derived构建期，而不是使用采样、近似或降低完整性。

TDD与回归证据：

```text
RED:       测试先调用不存在的Gfx count/page/point API，编译按预期失败
Reader:    Dispatch/Entity/Link count=1/1/1；page和ID点查均返回精确对象
Query:     job.gfx.statistics不再建立完整Gfx和Job vector
GREEN:     Trace Session Inventory tests passed；固定组合回归6/6
```

本子阶段未启动真实30分钟转换，也未修改录制协议、Unity或Player。

### 2026-09-01 Gfx Chain 有界邻接读取（N30.6B 子阶段）

状态：**Passed（synthetic correctness，`job.gfx.statistics`全局摘要仍待独立收敛）**。

- I/O/Gfx index schema从8升级到9；新增`dispatchId→Dispatch ordinal`精确posting，和既有Entity ID、Parent、Link Source/Target posting共同组成双向邻接读取基础。
- Session Reader新增`GfxChain(rootId, maxNodes)`；每个已访问节点只点读关联Dispatch、Entity、子Entity及正反向Link，不读取完整Dispatch/Entity/Link vector。
- Reader同时限制节点数和边数；节点容量或边预算不足时返回`truncated=true`，不把局部结果伪装为完整链。返回Link只包含两端均已进入结果的边。
- Session `job.gfx_chain`在任何`GetJobs()/GetGfx*()`全量物化之前进入专用路径；Gfx邻接由磁盘posting完成，仅对有界的visited ID执行Job点查。
- Legacy Worker仍使用通用内存实现；输出继续保持Job、Dispatch、Entity、Link和visited/truncated字段语义。
- Final Audit新增Dispatch-ID posting的顺序、ordinal边界及实际Dispatch ID校验；错误posting会阻止Session发布。

TDD与回归证据：

```text
RED:       测试先调用不存在的GetGfxChain，编译按预期失败
Reader:    root Dispatch 300 → Entity 301 → Link 300→301
Query:     job.gfx_chain(Dispatch 300)返回1 Dispatch、1 Entity、1 Link、2 nodes
GREEN:     Trace Session Inventory tests passed；固定组合回归6/6
```

本子阶段未启动真实30分钟转换；`job.gfx.statistics`已在后续“Gfx Statistics 分页与点查”子阶段改为有界内存的精确分页扫描，其真实长Trace墙钟仍由N30.7验证。

### 2026-09-01 Job Schedule-Time Posting 与 Search 分页（N30.6B 子阶段）

状态：**Passed（synthetic correctness，真实长Trace墙钟与容量待N30.7集中门禁）**。

- Job page schema从4升级到5，posting schema从3升级到4；新增按`(scheduleNs, JobId)`严格排序的精确Schedule posting，以及按`(JobId, scheduleNs)`排序的反向审计投影。
- 有符号Schedule时间通过翻转符号位编码为无符号排序键，保持完整`int64_t`时间顺序；相同时间以Job ID稳定排序。
- Schedule顺序事实与六类Job延迟在同一次最多4096个Job的有界分页复原中生成，避免为同一批Job执行第二次全量磁盘复原扫描。
- Session Reader新增`ScanBySchedule(offset, limit)`，只读取目标Schedule posting页，再按Job ID点查并按posting顺序复原DTO；不会为分页结果物化全部Job。
- Session `job.search`改用Schedule顺序和raw-offset游标执行`ScanFiltered`；文本、kind、state和source过滤仍保持增量扫描。Legacy Worker默认实现显式排序`(scheduleNs, JobId)`，与既有查询语义一致。
- Final Audit验证forward/inverse布局、严格顺序、Job成员、双向存在关系，并从最终Schedule事实重算每个Job的稳定Schedule时间；checksum正确但时间或成员错误仍会阻止Session发布。

Synthetic使用非单调ID证明查询不是偶然按ID有序：

```text
Schedule order: Job 600 @ 104 ns → Job 500 @ 110 ns
Job-ID order:   Job 500 → Job 600
Reader ScanBySchedule(0,2)=[600,500]
Query job.search(limit=2)=[600,500]
Trace Session Inventory tests passed；固定组合回归6/6
```

本子阶段未启动真实30分钟转换，也未修改录制协议、Unity或Player。
