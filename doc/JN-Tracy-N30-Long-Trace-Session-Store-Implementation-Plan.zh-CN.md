# JN Tracy N30 长时间 Trace Session Store 实施计划

## 1. 目标与固定决策

N30 从 N29 提交 `15ceef123bed1ae0a87ed0cb4af908c3b19e83e8` 继续开发，解决长时间 `.tracy-stream` 必须完整载入 `tracy::Worker`、转换内存随事件总量增长的问题。

统一流程：

```text
.tracy-stream
→ Source Inventory
→ Canonical Session Shards
→ Mandatory Derived Index
→ Final Audit
→ <Capture>.jn-trace-session
```

所有录制默认走 Session Store，不区分短录制和长录制，不需要格式参数。传统 `.tracy` 由完成的 Session 按时间或 Frame 范围导出。

权威关系：

```text
.tracy-stream      = 原始录制证据，默认永久保留
Session Canonical  = 唯一转换后事实来源
Derived Index      = 可从 Canonical 重建的确定性分析结果
```

N29 GPU Resource Analysis 作为强制派生域并入 Session。新 Session 不复制独立 N29 Raw Sidecar；历史 `.tracy` 继续支持独立 `.jn-gpu-resource-analysis`。

全局优先级：

```text
正确性 > 可恢复性 > 查询完整性 > 转换速度 > 磁盘占用 > 使用便利性
```

无法证明正确的数据不得发布、近似、静默丢弃或伪装为完整。

## 2. 分支、范围与版本

实施位置：

```text
Branch:   feature/JNTracy-N30-LongTraceSessionStore
Worktree: C:\CodeProjects\GodotProjects\tracy-0.13.1-n30-long-trace-session
Base:     15ceef123bed1ae0a87ed0cb4af908c3b19e83e8
```

只修改 N30 worktree，不修改或合并 `dev`、N29、Unity、PackageRepo 和 Player。构建只使用 `build-n30-*`。

版本：

```text
Tracy Protocol             90（保持）
JN trace section           12（保持）
GPU Catalog schema          1（保持）
GPU Analysis raw schema     1（保持）
Trace Session Store schema  1（新增）
Canonical Event schema      1（新增）
Derived Index schema        1（新增）
Query API                 1.34
```

Canonical schema 变化必须从 stream 重建；Derived schema/算法变化只重建对应 generation。当前和上一个成功 Derived generation 默认保留。

## 3. Session 产物和公共入口

构建目录：

```text
<Capture>.jn-trace-session.building.<uuid>\
├─ build-state
├─ inventory\
├─ canonical\
├─ global\
├─ derived\
├─ checkpoints\
└─ audit\
```

完成后原子发布：

```text
<Capture>.jn-trace-session\
├─ CURRENT
├─ manifest
├─ generations\<generation-id>\
│  ├─ canonical\
│  ├─ global\
│  ├─ derived\
│  └─ audit\
└─ status
```

`.building` 只允许查询进度，Profiler/Query/MCP 不得读取 Trace 数据。Session 可以整体复制、改名和移动，内部只保存相对路径。相同 source identity 可以发布新 generation；不同 identity 返回 `identity_mismatch`。

统一转换入口：

```powershell
tracy-stream-convert.exe -i <Capture>.tracy-stream
```

默认输出 `<Capture>.jn-trace-session`。`-o` 只覆盖 Session 目录，不选择格式。支持 `--resume`、`--restart-generation` 和 `--status`。

局部导出：

```powershell
tracy-session-export.exe `
  -i <Capture>.jn-trace-session `
  -o Window.tracy `
  --time-begin <ns> `
  --time-end <ns>
```

也支持 FrameSet/Frame 范围。导出必须包含自包含字典、Thread/GPU Context、范围内全部域、起点存活状态、FrameImage、GPU Catalog/Pass关系，以及 `open_before_export_range`、`open_after_export_range` 边界语义。预计需要超过 16 GiB 内存时拒绝，不截断或降级。

## 4. 身份、状态与取消

完整身份包含 source SHA-256、大小、committed revision、Protocol、capture identity/end state、Converter SHA-256、Session/Canonical/Derived schema 和配置 hash。路径、mtime 和分段 hash 只能快速筛选，不能代替完整 SHA-256。

内部状态：

```text
InventoryBuilding / InventoryFailed / CapacityPreflight
CanonicalBuilding / CanonicalPaused / CanonicalFailed
DerivedBuilding / DerivedFailed / FinalAuditing
CancelledResumable
```

可发布状态：

```text
Complete
CompleteSourceDegraded
```

不可发布状态：

```text
InvalidSource / InvalidConverterOutput / InvalidChecksum
InvalidCoreGap / InvalidCapacity / InsufficientDisk / IdentityMismatch
```

Converter 造成的不完整阻止发布。源数据已声明的缺失忠实保存并按域失效，Session 可发布为 `CompleteSourceDegraded`；受影响的跨域分析不得生成 Exact 结论。

取消规则：第一次 Ctrl+C 请求 GracefulCancel、完成最小事务并写 checkpoint；短时间内第二次 Ctrl+C 只提示正在安全收尾；真正强制退出必须显式 `--force-abort` 或由操作系统终止。恢复只使用最后校验通过的 checkpoint。

## 5. 三阶段转换

### 5.1 Source Inventory

完整顺序扫描 stream，但不构建完整事件 Worker。记录完整 SHA-256、revision、client/server record、各域计数、record ordinal/位置/时间/依赖、迟到事件、dictionary/blob依赖、capture结束状态、安全Shard边界和容量估算。大型索引直接写 immutable runs。

### 5.2 Canonical Shard Build

```text
OfflineReplayTransport
→ Shared Protocol Decoder
→ CanonicalSessionSink
→ Global dictionaries + aligned time shards
```

协议解析与内存存储解耦，提供 `FullWorkerSink`、`InventorySink`、`CanonicalSessionSink` 和 `WindowExportSink`。Canonical 只保存无损事实，不计算 Top N、Inclusive Working Set 等派生结论。

### 5.3 Derived Index与Final Audit

只读取 Canonical，不重读 stream、不重跑 GPU Catalog resolver。生成 Frame/time、Thread/Zone、Job、Sampling/Context Switch、Memory、Source/Callstack、FrameImage、N29 GPU Resource Analysis 和 Domain quality。

发布前必须满足：

```text
Inventory source count
= Canonical committed count
= Derived source count
```

## 6. Shard、全局表与Checkpoint

所有域使用同一全局时间边界：

```text
目标未压缩数据 256 MiB
软上限         384 MiB
硬上限         512 MiB
最短跨度         5 秒
最长跨度        30 秒
```

达到目标且超过 5 秒、达到 30 秒或接近软上限时，在合法记录/Blob边界提交。单个不可拆记录超过硬上限时生成 `oversized` shard并记录原因。

每个 Shard 保存 Session/generation/shard/domain、时间、source record范围、计数、压缩/未压缩字节、codec、SHA-256 和前后 checkpoint hash。索引采用 immutable sorted runs、external merge 和 mmap sparse index，不引入外部数据库。

全局表保存 Strings、SourceLocations、Callsites、Callstack、Thread/Process、GPU Context、FrameSet、MemoryPool、稳定 Job/Gfx/GPU Resource ID、AppInfo 和 Capability。

Checkpoint保存 open CPU/GPU zone、active job/dependency、live allocation/resource、GPU pointer lifetime、open I/O、pending relation、dictionary generation、GPU timestamp calibration、Frame/Thread状态和 decoder/source ordinal。

## 7. 数据域

Canonical 必须覆盖当前 TraceSource 可访问的全部域：Frame、CPU/GPU Zone、Thread/Process/Fiber、Job/Dependency/Stage/Wait/Gfx、Message/Plot/Lock、Sampling/Kernel/Ghost/Context Switch、Hardware Sample、CPU/GPU Memory、GPU Catalog/Allocation/Heap/View/Range/Reference、I/O、RuntimeDomain、C#/Lua Source Stack、Symbol/Source/Callstack、FrameImage、Crash、Quality、JN Relation和其他 section 12 数据。

源中不存在返回 `present=false`；存在但失效返回明确 invalid reason。

## 8. 原子提交、恢复和资源门禁

Shard事务：

```text
Building → PayloadWritten → CheckpointWritten → CountVerified
→ SHA256Verified → FileBuffersFlushed → AtomicRename → BuildStateUpdated
```

Manifest使用临时文件、flush、原子rename和`CURRENT`原子替换。恢复时验证source、工具/schema/config、全部已提交Shard和最后checkpoint；任何不一致拒绝恢复。

Converter预算：

```text
稳态目标  8 GiB
软上限   12 GiB
硬上限   16 GiB
```

达到软上限时停止预取、提前封存、flush并回收cache；预计超过硬上限时checkpoint并返回`resource_limit`。禁止采样、丢事件、缩栈或近似集合。

磁盘预算：

```text
MaxSessionStoreGiB=256
MinimumFreeReserveGiB=64
MinimumFreeReservePercent=10
PreflightRequired=true
AllowUnboundedGrowth=false
```

Inventory预检计算 Canonical + Derived + Temporary + max(64 GiB, volume 10%)。不足时不进入Stage 2；运行中不足时安全暂停，永不删除stream、Shard或成功generation。

每个building Session只允许一个writer，使用PID、进程创建时间、lease generation和heartbeat。默认 BelowNormal，线程数 `min(logical cores/2, 8)`，顺序I/O优先，支持Pause/Resume。

## 9. N29 GPU Resource Analysis整合

新Session：

```text
canonical/gpu/*                  = GPU事实
derived/gpu-resource-analysis/* = N29确定性索引与摘要
```

复用 N29 Builder、Exact summary、Resource↔Pass、Allocation/Heap/Range/Frame索引、checkpoint/generation/checksum、Reader、Profiler Controller和Query/MCP方法。

替换：

```text
旧：Full Worker.GetJnTraceData() → WriteGpuAnalysisRawSidecar
新：Session Canonical GPU reader → BuildGpuResourceAnalysisDerived
```

Legacy `.tracy` 仍支持独立 sidecar。新Session不复制Raw GPU事实。Derived损坏时禁止回退到完整内存Snapshot，只允许从Canonical重建；首次发布时GPU Derived失败会阻止Session发布。

## 10. Query、MCP、Profiler

新增 `TraceSourceKind::Session`、`SessionTraceSource` 和 `SessionReadView`。Session不得包装成完整snapshot Worker。对Jobs、Relations、Symbol/Source、GPU Catalog、Memory、Sampling和FrameImage等全量vector接口增加分页/范围版本。

Query 1.34：Session目录可直接输入，只打开`CURRENT`完整generation；`.building`只能调用`session.build.status`；支持分页、取消、时间/Frame范围，单响应≤16 MiB。所有结果返回identity、generation、domain status和exactness。MCP内存目标8 GiB、硬上限16 GiB。

LTS-1完成时提供完整Session、Query/MCP、N29 GPU分析和局部`.tracy`导出。

LTS-2时Profiler增加`Open Trace Session`，读取Manifest/摘要、当前可视范围、相邻预取和LRU。用户看到连续时间线而非Shard；GPU Memory窗口读取N29派生域。GUI主线程不得做完整SHA、全表扫描、集合union或Shard构建。Profiler Session cache目标4 GiB、硬上限8 GiB，Profiler+helper≤16 GiB。

## 11. 实施阶段

1. **N30.0**：隔离worktree、计划和Oracle基线。
2. **N30.1**：Schema、身份、原子Shard/generation存储。
3. **N30.2**：Inventory、源质量和容量预检。
4. **N30.3**：Canonical sink、checkpoint、恢复、取消和资源门禁。
5. **N30.4**：全部Canonical域和逐域差分。
6. **N30.5**：Mandatory Derived、N29整合和Final Audit。
7. **N30.6**：Query/MCP 1.34、SessionTraceSource和局部导出。
8. **N30.7**：LTS-1短中Trace、真实30分钟、2小时Synthetic和故障注入验收。
9. **N30.8**：Profiler Session后端、懒加载、LRU和跨域导航。
10. **N30.9**：LTS-2一致性/性能验收、报告和提交。

每个生产行为遵循TDD：先建立会因能力缺失而失败的测试，确认RED，再实现最小GREEN，最后重构并跑全套回归。

## 12. 验收

短/中Trace必须将传统Full Worker `.tracy` 与Session逐域对比，Exact域100%一致。覆盖跨Shard Zone、Job、Allocation、pointer reuse、GPU Resource、I/O、迟到事件、FrameImage、oversized Blob和低密度时间段。

故障注入覆盖Inventory、payload、checkpoint、checksum、rename、Derived、GPU Index、Final Audit和`CURRENT`切换；已提交Shard必须保持可读，未提交Shard不得进入manifest，恢复不得重复或丢失事件。

固定真实30分钟输入：

```text
C:\Users\Admin\Documents\JN-Unity-T3\N29-Autonomous-30m\Run-20260830-220012\Admin-HighEvidence-30m.tracy-stream
Size:   19,122,822,908 bytes
SHA256: 2AC46C53257CE9027EC67E098FC15070FB911243F6CD311A166EB97547848068
```

要求完成Session、不生成完整中间`.tracy`、Converter≤16 GiB、Sampling/Context Switch/GPU Memory/Catalog/Pass/Range可查、Profiler/MCP同generation、原stream不变。

2小时等价Synthetic验证无Frame/时长上限、内存不随总事件持续增长、输入翻倍核心构建时间≤2.5倍、Shard线性增长、Cancel/Resume和磁盘恢复。

性能门槛：

| 项目 | 门槛 |
|---|---:|
| Converter稳态/硬内存 | 目标≤8 GiB / ≤16 GiB |
| Profiler Session cache | 目标≤4 GiB，硬上限8 GiB |
| Profiler+helper | ≤16 GiB |
| MCP | 独立目标8 GiB，硬上限16 GiB |
| Session默认上限 | 256 GiB |
| 安全磁盘预留 | max(64 GiB, 10%) |
| Session摘要打开 | ≤2秒 |
| 冷/热窗口 | ≤5秒 / ≤2秒 |
| 单Frame/Resource | ≤2秒 |
| 跨域Query | ≤5秒 |
| 取消响应 | ≤2秒 |
| Exact一致性 | 100% |
| Core gap/checksum failure | 0 |

墙钟时间按Inventory、Canonical、Derived和Audit分别记录；不得连续120秒无进度且无明确状态。

## 13. 完成定义与延期

完成要求：统一Session输出、有界内存、全数据域Canonical、N29 mandatory derived、不可见中间状态、安全恢复、原stream保留、Query/MCP/局部导出、Profiler直接Session、真实30分钟和2小时Synthetic通过、文档与提交完整。

延期：录制期分片、查询仍在写入的stream、Live Session、Unity/Package/Player改动、新Protocol、新GPU Catalog语义、远程Session服务、自动删除stream、真实60分钟/2小时Gameplay。

预计：LTS-1 15～25个工作日，LTS-2 15～25个工作日，集中验收5～10个工作日，总计35～60个开发工作日。
