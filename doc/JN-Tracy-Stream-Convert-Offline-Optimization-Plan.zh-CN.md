# JN Tracy StreamConvert Offline 性能优化实施计划

## 1. 目标、边界与隔离门禁

将当前 `.tracy-stream -> .tracy` 转换从 localhost 锁步重放改为隔离的离线转换路径，在不改变 `.tracy` 文件格式和数据语义的前提下，将 1.796 GB、约 2.06 亿 protocol events 的基准文件转换时间从当前超过 30 分钟降至：阶段可用不超过 10 分钟，正式完成不超过 5 分钟，进取目标不超过 3 分钟。每项结果运行三次并取中位数。

正确性和性能门禁：

| 项目 | 门禁 |
|---|---:|
| 所有数据域语义一致性 | 100% |
| 转换数据丢失 | 0 |
| Profiler 打开时间回退 | <=10% |
| Query 索引时间回退 | <=10% |
| Fast 输出大小 | <= Legacy 的 125% |

基准输入固定为：

```text
C:\Users\Admin\Documents\JN-Unity-T3\PlayerCaptures\StreamConvertOptimization\Manual-HighEvidence-20260820-160336.stream-convert-opt.tracy-stream
```

其大小为 `1,796,451,880` 字节，SHA-256 为 `697060EB63A1AA8B4F4057CC8644698242836FE489DC5BF12E0B6953DB747842`。

实施只允许发生在：

```text
C:\CodeProjects\GodotProjects\tracy-stream-convert-opt
feature/JNTracy-StreamConvert-Performance
```

隔离规则：

- 原仓库 `C:\CodeProjects\GodotProjects\tracy-0.13.1` 及其 `dev` 工作树保持不变。
- 使用独立 build 目录，不安装或覆盖任何稳定 Tracy 二进制。
- 禁止运行 `git gc`、`git prune`，禁止修改共享 Git hooks/config。
- 测试只读取 `StreamConvertOptimization` 下的物理副本。
- `.tracy`、snapshot map、scan cache、spill 和报告只写入隔离测试目录。
- 不修改 PATH、注册表、文件关联、计划任务或全局环境变量。
- Legacy/compare 使用独立 localhost 端口；Offline 不创建监听端口。

本轮仅正式支持 Windows x64、构建时当前 Tracy 协议（本次实测为 Protocol 88）、当前 JN schema 和 NTFS 本地磁盘。不改变 `.tracy` 文件格式，不为 Query/MCP 新增 stream 后端，不兼容旧 `.tracy-stream` 协议，不修改 GUI，不实现完整 Worker checkpoint/resume。协议号必须从 `TracyProtocol.hpp` 和输入 journal header 自动验证，文档不得再次把已过期协议号作为运行时常量。

## 2. OfflineConvert 架构

### 2.1 Worker 模式隔离

新增严格隔离模式：

```cpp
enum class Worker::Mode : uint8_t
{
    Full,
    ProtocolOnly,
    OfflineConvert
};
```

`Full` 和 `ProtocolOnly` 行为保持不变。Offline 专用预分配、缓存、批处理和写盘策略只能在 `OfflineConvert` 中启用。Legacy localhost replay 只保留在开发/验证构建中作为 oracle，正式 converter 不允许自动回退。

### 2.2 数据流

```text
ScanJournal
-> 校验构建时当前协议（本次为 Protocol 88）与 committed revision
-> 建立 Client/Server record 索引与数据域直方图
-> 有界并行读取、LZ4 解压和批次解析
-> 按 recordSequence 恢复稳定顺序
-> 进程内响应 Worker 查询
-> 有序提交状态事件，独立域并行构建
-> Worker 模型完成
-> 并行写出和压缩
-> 重新打开并深度校验
-> 原子发布 .tracy 和 snapshot map
```

Server transcript 必须全量验证：每条必需记录精确匹配和消费一次；保留依赖与必要顺序；重复、缺失、孤立或 payload 不一致立即失败；禁止以“不影响模型”为由跳过数据。

### 2.3 并行、确定性与资源保护

默认线程数为 `max(1, logicalProcessorCount - 1)`，进程优先级保持 Normal。读取、解压、批次解析、字符串预处理、调用栈规范化、Sampling、Context Switch 和独立输出流压缩可以并行。Frame、Zone、线程状态、Job、GPU Pass、显存生命周期等状态机事件必须按稳定序列提交。

所有并行结果携带 record sequence、event sequence、time、thread 等稳定键；不同线程完成顺序不得改变模型。相同输入用 1 线程和 N-1 线程产生的分域语义哈希必须一致。

内存策略：进程软上限为物理内存 60%；系统空闲下限为 `max(8 GiB, 15%)`；Commit 使用率 85% 时进入压力状态。依次降低预取、降低并发、提前 flush、启用按需 spill，禁止静默丢数据。

临时缓存位于输入副本目录的 `.jnconvert-cache/<stream-identity>`。scan cache 仅含 offsets、record 类型、依赖索引和直方图；spill 只含可重新生成的中间批次。缓存带 sequence、长度、checksum 和 schema，成功后删除；安全取消可保留 scan cache。

### 2.4 高频域与写盘

Sampling、Context Switch 和 Callstack 使用扫描直方图预分配、稳定字典、线程映射缓存、批量 append 和离线派生分析。所有事件的时间、线程、CPU、调用栈和命中次数必须精确保留。优化不得改变 Full/live Worker 的默认处理时机。

保持现有 `.tracy` 格式，提供：

| Preset | 行为 |
|---|---|
| fast | 正式默认，优先 Zstd level 1 |
| balanced | 根据实测使用 level 2 或 3 |
| legacy | 当前 Zstd level 3 / 4 streams |

独立输出流允许并行压缩。Fast 文件若超过 Legacy 大小 125%，依次评估 level 2、level 3。若最终唯一剩余瓶颈是现有 `Worker::Write` 或文件格式，则停止并提交报告，不擅自升级 `.tracy` 格式。

## 3. CLI、进度和失败语义

正式 CLI：

```text
tracy-stream-convert
  -i <input.tracy-stream>
  -o <output.tracy>
  [-f]
  [--compression fast|balanced|legacy]
  [--threads auto|N]
  [--require-clean-end]
  [--progress-json <path>]
  [--report-json <path>]
  [--no-cache]
  [--purge-cache]
  [--keep-failed-output]
  [--disk-budget <bytes>]
```

默认 Offline、fast、auto threads；不覆盖已有输出；`-f` 只允许在新输出校验完成后原子替换。正式 CLI 移除 `-p/--port`，不提供忽略损坏、绕过空间门禁或自动 Legacy fallback。

开发构建额外提供 `--mode offline|compare|batch-compare` 和 `--corpus`。

进度分 Scan、Decode、Build、Write、Validate 五阶段，每两秒显示总体与阶段百分比、运行时间、ETA、events/s、MB/s、当前数据域、线程数、内存、临时文件大小和降级状态。`--progress-json` 原子更新，供脚本/MCP轮询。

磁盘预检必须覆盖预计临时 `.tracy`、最坏 spill 和 2 GiB 安全余量。空间不足时在完整模型构建前失败；转换期间持续监控；不自动删除任何用户文件。

输出流程：

```text
output.tracy.converting
-> Finish
-> 重新打开校验
-> 原子发布 output.tracy
-> 原子发布 snapshot map
```

输入始终只读。失败、取消或崩溃不得覆盖有效输出。snapshot map 绑定有效 revision，只在正式输出发布后更新。默认正式产物只有 `.tracy` 与 snapshot map，JSON 报告按需生成。

对异常 stream，转换全部已提交且校验通过的 prefix；只允许忽略最后 commit 后的不完整尾部，并记录 `capture_complete`、revision、ignored tail bytes、last valid event time 和 recovery reason。committed 区域内任何损坏都必须失败。`--require-clean-end` 用于严格验收。

第一次 Ctrl+C 协作取消并清理，目标 10 秒、最大 30 秒；第二次立即终止，允许留下可识别临时文件。取消退出码 130。重试只复用有效 scan cache，不恢复完整 Worker 状态。

## 4. 实施阶段

### SC.0 基线

- 落盘计划，记录两个 worktree 身份、测试文件哈希、编译器和硬件。
- 为 Scan、dependency、Client replay、Server verification、Worker ingest、deferred analysis、Write 和 snapshot map 增加计时。
- 记录 wall/CPU time、峰值内存、事件数、吞吐和输出大小。
- 压力文件运行三次 Legacy 基线；不得改变转换语义。

### SC.1 Offline 与进程内 transcript

- 新增 OfflineConvert 和专用配置。
- 将 scan、payload、Server dependency 和 transcript match 拆成可测组件。
- 实现进程内 Worker transport/response provider。
- 先用单线程证明 Offline 与 Legacy 分域语义一致。
- Full/live Worker smoke test 必须通过。

### SC.2 有界并行 Decode/Build

- 实现 batch scheduler、sequence reorder buffer、压力反馈和 spill。
- 并行读取/LZ4/解析，有序提交状态事件。
- 增加 1 线程/N-1 线程确定性测试。
- 压力文件中位数必须先达到 10 分钟以内，否则继续定位而不掩盖问题。

### SC.3 高频域优化

- 针对 Sampling、Context Switch、Callstack 增加预分配、字典、批量 append 和 deferred analysis。
- 每项优化保留前后耗时证据。
- 事件、时间、线程、CPU、栈和命中次数必须完全一致。

### SC.4 Write、CLI 与原子发布

- 实现三个压缩 preset、独立流并行压缩、进度、ETA、JSON、磁盘预检、取消和原子发布。
- Fast 大小、Profiler 打开、Query 索引门禁全部通过。
- 正式构建隐藏 Legacy/compare 入口。

### SC.5 分域语义比较器

规范化并比较：Frame、CPU/GPU Zone、Callstack、Sampling、Context Switch、Job、GPU Memory、Resource Graph、I/O、FrameImage、Plot、Message 和 quality counters。允许内部 ID、字符串表编号和压缩块布局不同；临时 ID 映射为稳定语义键；时间戳和数值精确比较。

报告必须包含 overall、domain hash、record count、first mismatch 和上下文，并能够检出人为注入的少事件、错时间、错关系、错栈及错质量计数。

### SC.6 最终构建与验收

计划四次构建：基线计时增量构建、Offline/并行增量构建、CLI/比较器整合构建、最终 Windows x64 clean Release。

开发期只运行小型 synthetic 和少量真实 trace；测试脚本与 manifest 保留在 test 范围但不进入正式发布，大型 trace 不进 Git，开发临时输出最终清理。

最终一次完整批量验收自动筛选构建时当前协议（本次为 Protocol 88），覆盖正常结束、重连、强制停止、截断、错误注入及所有主要数据域，包含 1.796 GB 压力文件。旧协议明确排除。

## 5. 最终验收、发布与回滚

每个最终语料必须完成 Offline 转换、重新打开、transcript 全量消费、分域语义比较、Query doctor/quality/domain/relation、snapshot map 身份验证，并确保无 dropped、unexplained orphan、overflow 或 silent downgrade。

代表性 trace 通过 MCP 验证 Frame 到 CPU/Job、GPU Pass、GPU Memory/Resource Graph、Sampling/Context Switch、I/O/FrameImage 的关系。小型、典型和大型 trace 各进行一次 Profiler 打开 smoke；崩溃、无法打开或明显缺失均为阻断失败。

性能验收在固定机器、本地 NTFS 和同一输入下运行三次取中位数，统计完整 Scan、Decode、Build、Write、Validate 和 snapshot map。达到 5 分钟且全部语义门禁通过才完成；5-10 分钟继续优化最大瓶颈；超过 10 分钟不得发布。

正式发布包只包含构建时当前协议（本次为 Protocol 88）配套的 converter、profiler、query、必需 DLL/PDB 和 build identity。正式用户默认命令仅为：

```powershell
& $converter -i $stream -o $trace
```

所有实现按 SC.0-SC.6 分阶段提交在独立分支。任一阶段可回退到上一提交。OfflineConvert 通过最终门禁前不得替换稳定工具；方案失败时删除独立 worktree/branch即可恢复，不触碰原始 trace。
