# JN Tracy N30A 确定性全量验收器设计

状态：设计已确认，等待用户评审后进入实现计划与实现。

日期：2026-09-06。

## 1. 目标

本设计为已经完成的 N30A「全域可信 Session + Mandatory Derived + Query/MCP」建立一套可重复、可审计、无需 AI 参与判定的自动验收系统。

验收器由 Python 标准库编写，但不重新实现 Tracy Protocol、Session Reader 或 GPU Catalog 算法。Python 只负责：

1. 固定输入、工具和配置身份。
2. 调用当前 N30 Converter、Audit、独立 GPU Verifier 和 Query。
3. 保存原始响应与进程指标。
4. 使用固定 Oracle 和比较规则生成 Pass/Fail。
5. 输出机器可读结果、JUnit 和中文验收报告。

最终权威关系为：

```text
Protocol 90 .tracy-stream
  → N30 Converter / Session Audit / GPU Verifier / Query 1.34
  → 原始 JSON/NDJSON 与审计结果
  → Python 任意精度比较器
  → acceptance-result.json
  → 确定性 Markdown/JUnit 报告
```

AI 可以在验收结束后解释报告，但不能修改指标、忽略失败或改变最终判定。

## 2. 固定范围

### 2.1 本轮验收 A 包含

- 从当前 N30 HEAD 独立构建验收工具。
- Python 验收器自身测试。
- 真实短 Trace 的 Traditional Full Worker 与 Session 全域差分。
- 固定真实 30 分钟 stream 的全新冷转换。
- 新 Session、既有 Session、A7 Golden Oracle 三方比较。
- 全量 Canonical Audit。
- GPU Resource/Allocation/Pass/Range 独立 Verifier。
- Query 1.34 全域、分页、范围、跨域关系和取消测试。
- Cancel/Resume、崩溃恢复、身份、checksum、writer lease、内存和磁盘门禁。
- 两小时逻辑时间跨度 Synthetic。
- `1x/2x/4x` 有界数据密度 Synthetic。
- 转换、Query、磁盘和内存性能记录。
- 机器结果、中文报告和证据哈希。

### 2.2 明确延期

以下能力不参与验收 A 的 Pass/Fail：

- Session 语义数据局部导出 `.tracy`，状态为 `DEFERRED_OUT_OF_SCOPE`。
- Profiler 直接打开完整 Session 时间线，状态为 `DEFERRED_OUT_OF_SCOPE`。
- Profiler 的 GPU Memory & Resources Session 后端人工验收。

这些延期项不得在报告中写成已通过。

### 2.3 不包含

- 不修改 Protocol 90、JN section 12、Canonical schema 或 GPU Catalog schema。
- 不修改 Unity、PackageRepo、Player 和录制配置。
- 不运行真实 30 分钟或两小时 Gameplay 录制。
- 不自动修复验收发现的 N30 Bug。
- 不合并或推送 N30 分支。
- 不删除任何原始 stream、正式 Session 或验收 scratch。

## 3. 当前固定基线

### 3.1 Git

```text
Repository:
C:\CodeProjects\GodotProjects\tracy-0.13.1-n30-long-trace-session

Branch:
feature/JNTracy-N30-LongTraceSessionStore

Design baseline HEAD:
7eb9e55de1fdc01b61646e7d40dbf1a731003f00

Subject:
N30.A7 Audit trusted 30-minute Session
```

既有未跟踪目录 `build-n30-stream-tests/` 必须保持不变。正式验收使用新的 `build-n30-acceptance-*` 目录。

### 3.2 固定 30 分钟输入

验收使用已经复制到独立目录的原始录制：

```text
C:\Users\Admin\Documents\JN-Unity-T3\
N30A-Full-Acceptance-20260906\source-30m\
Admin-HighEvidence-30m.tracy-stream
```

固定身份：

```text
Size:
19,122,822,908 bytes

SHA-256:
2AC46C53257CE9027EC67E098FC15070FB911243F6CD311A166EB97547848068
```

`source-30m` 目录中的九个已复制文件均已和原位置做 SHA-256 对照，清单位于同目录 `COPY-MANIFEST.md`。验收开始时必须重新校验，且以该副本作为冷转换输入。

### 3.3 既有 A7 Session

```text
C:\Users\Admin\Documents\JN-Unity-T3\N29-Autonomous-30m\
Run-20260830-220012\Admin-HighEvidence-30m.jn-trace-session
```

固定已知值：

```text
CURRENT generation: n30-1788306859373994-47656
Canonical shards:   402
Indexed records:    2,285,222,848
Generation files:   1,286
Generation bytes:   202,134,780,631 bytes
```

既有 Session 只读使用，不复制、不重建、不切换 `CURRENT`。

### 3.4 A7 全域 Golden

| 数据域 | Golden 数量 |
|---|---:|
| Frame / FrameSet | 580,182 / 5 |
| CPU Zone | 234,691,471 |
| GPU Zone | 36,837,665 |
| Job | 18,578,994 |
| Gfx Entity / Link | 63,579,905 / 106,965,570 |
| Sampling | 206,122,329 |
| Context Switch | 104,697,072 |
| Memory Event / Pool | 1,864,438 / 9 |
| Relation | 125,513,195 |
| Message / Plot | 141,012 / 48 |
| FrameImage | 1,173 |
| I/O Request / Stage | 26 / 52 |
| Callstack payload / frame | 751,825 / 431,956 |

### 3.5 A7 GPU Golden

| 指标 | Golden |
|---|---:|
| Resource | 1,769,168 |
| Allocation | 1,769,007 |
| Pass | 27,375,840 |
| Range | 43,884,638 |
| Direct members | 357,965,580 |
| Inclusive / Resource-Pass relations | 667,721,122 |
| Forward relation hash | 5,854,663,396,778,239,566 |
| Reverse relation hash | 5,854,663,396,778,239,566 |
| Verifier report hash | 9,444,475,405,900,354,442 |
| `mismatch_count` | 0 |
| EngineKnownPhysical peak | 3,848,863,744 bytes |
| Peak time | 1,859,905,005,679 ns |

### 3.6 已知源端退化

该输入的合法最终状态是 `CompleteSourceDegraded`，不是 `Complete`：

| 原因 | Golden 数量 |
|---|---:|
| CPU Zone source clock inversion | 7,144 |
| process-scoped Scheduling source gap | 12,931,785 |
| GPU Resource identity gap | 140 |
| GPU Reference identity gap | 1,334 |
| GPU Catalog producer dropped / overflow | 159 / 159 |
| GPU Catalog coverage | 0.9999936778622713 |
| Converter-created unresolved | 0 |
| Verifier mismatch | 0 |
| committed checksum failure | 0 |

原因与计数必须完全一致。出现新原因、新缺失或数量变化均失败；不得将退化隐藏为 `Complete`。

## 4. 验收器架构

### 4.1 仓库代码布局

```text
tools\n30_acceptance\
├─ run_acceptance.py
├─ process_runner.py
├─ win32_metrics.py
├─ query_driver.py
├─ comparators.py
├─ oracle.py
├─ report_writer.py
├─ schemas\
├─ fixtures\
├─ tests\
└─ profiles\
   ├─ n30a-full.json
   └─ expected-n30a-a7.json
```

职责边界：

- `run_acceptance.py`：阶段编排、恢复和最终状态。
- `process_runner.py`：安全启动、Console Control Event、超时、进程树和日志。
- `win32_metrics.py`：通过 `ctypes` 读取 Working Set、Private Bytes、Commit、CPU、I/O、线程和 Handle。
- `query_driver.py`：启动正式 Query 进程并发送固定 JSON-RPC/NDJSON；不实现查询语义。
- `comparators.py`：精确整数、枚举、集合、哈希、性能门槛和排除字段。
- `oracle.py`：加载并验证 Golden、Schema 和输入绑定。
- `report_writer.py`：从机器结果确定性生成 Markdown、JUnit 和哈希清单。

仅使用 Python 标准库，不依赖 pip、NumPy、Pandas 或第三方数据库。

### 4.2 本机配置分离

版本控制中保存测试定义、门槛和 Golden，不保存机器绝对路径。本机配置保存到：

```text
C:\Users\Admin\Documents\JN-Unity-T3\
N30A-Full-Acceptance-20260906\config\local-inputs.json
```

该文件包含 N30 worktree、构建目录、短 Trace、30 分钟 source、既有 Session、新 Session、scratch 和报告目录。

### 4.3 运行入口

```powershell
python tools\n30_acceptance\run_acceptance.py `
  --profile tools\n30_acceptance\profiles\n30a-full.json `
  --inputs C:\Users\Admin\Documents\JN-Unity-T3\N30A-Full-Acceptance-20260906\config\local-inputs.json `
  --output C:\Users\Admin\Documents\JN-Unity-T3\N30A-Full-Acceptance-20260906
```

支持：

```text
--status
--resume
--from-phase <phase>
--rerun-phase <phase>
```

`--resume` 只有在源码、工具、输入、Profile 和 Golden SHA 全部一致时才允许复用已通过阶段。

## 5. 状态与总判定

每个测试只允许以下主状态：

| 状态 | 含义 | 允许总体验收通过 |
|---|---|---|
| `PASS` | 满足 Oracle 或门槛 | 是 |
| `FAIL_CORRECTNESS` | 事实、数量、关系、哈希或语义不一致 | 否 |
| `FAIL_INTEGRITY` | checksum、Shard、Manifest、Generation 或审计失败 | 否 |
| `FAIL_PERFORMANCE` | 超过硬性能或资源门槛 | 否 |
| `BLOCKED_ENVIRONMENT` | 输入、磁盘、权限或工具环境阻断 | 否；总体为未完成 |
| `HARNESS_ERROR` | 验收脚本自身失败 | 否；不能归咎于 N30 |
| `DEFERRED_OUT_OF_SCOPE` | 验收 A 明确延期 | 不参与本轮判定 |

目标未达成但没有突破硬门槛时可以增加 `warning` 注释，但不能用新的模糊主状态替代上述状态。

总结果：

- `PASSED`：全部 Required 测试为 `PASS`。
- `FAILED`：至少一个 Required 测试为 correctness、integrity 或 performance failure。
- `INCOMPLETE`：存在 environment block 或 harness error，证据不足以宣称通过。

## 6. Oracle 与比较模型

### 6.1 三层 Oracle

1. **真实 30 分钟 Golden Oracle**：绑定固定 source SHA、N30 Git、Protocol/Schema 和 A7 精确结果。
2. **真实短 Trace Differential Oracle**：Traditional Full Worker 与 Session 对同一输入做逐域语义比较。
3. **行为与资源 Oracle**：验证取消、恢复、身份、损坏、内存、磁盘、分页和规模增长不变量。

Golden 不是“当前结果自动更新”。修改 `expected-n30a-a7.json` 必须经过显式代码评审，并说明事实来源。

### 6.2 零误差数据

以下全部零误差：

- source SHA-256、大小和 committed revision。
- Protocol、Session、Canonical、Derived 和 Query Schema。
- Session identity 和 `CURRENT` generation 语义。
- 事件、实体、关系、Shard 和文件数量。
- checksum、关系哈希和 verifier hash。
- Domain status、exactness 和 unavailable reason。
- 原始整数时间戳和峰值字节。
- 已知源退化的原因和计数。

Python 使用任意精度 `int`。精确计数如果由 Query 输出为浮点数，判为接口错误。覆盖率通过原始分子/分母或有理数交叉相乘判断，显示小数不作为权威。

### 6.3 新旧 Session 三方比较

新冷转换 Session 必须同时满足：

```text
Inventory(source)
  == New Session Canonical/Audit
  == Existing A7 Session
  == A7 Golden
```

GPU 还必须满足：

```text
New GPU Derived
  == Independent GPU Verifier
  == Existing A7 GPU Derived
  == A7 GPU Golden
```

只匹配数量但关系哈希不同仍失败。

### 6.4 工具身份

工具 EXE SHA-256 全部记录，但不要求等于历史 A7 EXE SHA，因为 Windows 构建可能嵌入非确定性时间信息。Golden 绑定源码 commit、Schema 和构建配置；本次验收从 P2 开始锁定工具 SHA，中途任一工具变化会使 Run 失效。

### 6.5 明确排除字段

仅以下字段不参与语义相等判断：

- PID。
- 临时目录绝对路径。
- 报告生成时间。
- 单次进程启动墙钟时刻。
- 文件系统枚举顺序。
- 日志中的绝对路径。

所有排除项必须写入报告 `comparison_exclusions`。不得运行时临时添加忽略字段。

## 7. 阶段流水线与停止策略

| 阶段 | 内容 | 主要输出 |
|---|---|---|
| P0 Identity | 仓库、工具、输入、既有 Session 和磁盘预检 | `identity.json` |
| P1 Harness | fixture 自测比较器、进程监控、超时和报告 | `harness-selftest.json` |
| P2 Toolchain | 构建 N30 工具并运行现有单元测试 | `toolchain.json` |
| P3 Short Oracle | Full Worker 与 Session 的真实短 Trace 差分 | `short-differential.json` |
| P4 Fresh 30m | 固定 source 的全新 Session 冷转换 | `fresh-30m-conversion.json` |
| P5 30m Audit | 新旧 Session、Golden、Audit 和 GPU Verifier | `real-30m-audit.json` |
| P6 Query Matrix | 全域、分页、范围、跨域和取消 | `query-matrix.json` |
| P7 Recovery | Cancel/Resume、损坏、身份和 lease | `recovery-tests.json` |
| P8 Scale | 两小时逻辑跨度与 1x/2x/4x 密度 | `synthetic-scale.json` |
| P9 Report | 汇总 JSON、JUnit、Markdown 和证据哈希 | 最终报告 |

停止策略：

- P0 或 P1 失败立即停止。
- P2 失败停止全部依赖工具的阶段。
- P3 正确性失败使总结果确定为 FAILED，但仍可继续安全的只读 A7 审计以生成诊断证据。
- P4 或 P5 正确性失败后不执行耗时的 P8，仍生成完整失败报告。
- P6/P7 独立子域按依赖停止，不重复运行已经证明失败的路径。
- 正确性和性能分别报告；性能通过不能覆盖正确性失败。
- 验收器不自动修改源码、重建修复版或循环重试 Bug。

## 8. Query 验收矩阵

### 8.1 身份与能力

- Session、Canonical、Derived、Protocol 和 Query API。
- `CURRENT` generation。
- 每个 Domain 的 `present/status/exactness/reason`。
- `CompleteSourceDegraded` 及全部已知缺失。
- `.building` 只能查询 build status，不能读取 Trace 数据。

### 8.2 数据域

| 域 | 验收内容 |
|---|---|
| Frame/Thread | FrameSet、首中末帧、线程、时间范围 |
| CPU Zone | 父子、SourceLocation、跨 Shard 开放边界 |
| GPU Zone | Context、Queue、校准、Zone 层级 |
| Job/Gfx | Schedule、Dependency、Slice、Complete、Wait、Gfx 链 |
| Sampling | 用户态/内核样本、Callstack 和线程 |
| Context Switch | 区间、CPU、线程范围和 source gap |
| CPU Memory | Pool、Alloc/Free、存活集合和峰值 |
| GPU Memory | allocation、heap、residency、物理峰值 |
| GPU Catalog | resource、pass、range、allocation、双向关系 |
| Source/Stack | SourceLocation、Callsite、StackRef、unavailable reason |
| I/O | Request 生命周期、Stage 和字节数 |
| Message/Plot/Lock | 数量、时间范围和内容 |
| FrameImage | Frame、尺寸和 payload |
| Runtime/JN Relation | Domain state 和跨域关系 |

### 8.3 跨域链

固定验证：

```text
Frame → CPU Zone → Source/Callstack
Frame → Job Schedule → Slice → Complete/Wait
Frame → GPU Pass → Resource → Allocation/Heap
GPU Resource → GPU Pass
Allocation → Resource → GPU Pass
Frame → Sampling/Context Switch
Frame → FrameImage
```

### 8.4 确定性锚点

脚本不使用随机数。每个 generation 固定选择：

- 第一、正中和最后一个有效 Shard。
- 每个代表 Shard 的首尾 Frame。
- GPU physical peak 时间。
- 最小、中位和最大稳定 Resource ID。
- 最大 Allocation。
- fan-out 最大的 GPU Pass。
- 跨 Shard 生命周期对象。
- 已知 source-degraded 区间。
- 第一、中间和最后一张 FrameImage。

锚点写入 `anchors.json`，相同 generation 必须重复生成相同锚点。

### 8.5 全量与抽样边界

Python 不通过 Query 下载 22.85 亿条记录。完整性由 Canonical Audit 全量扫描和 GPU Verifier 全量扫描保证。Query 负责验证聚合结果、访问语义、确定性锚点、分页、范围和跨域联查。

代表性分页接口至少读取连续三页，并用不同 page size 重读相同逻辑范围；合并结果必须无重复、无遗漏且顺序稳定。

现有 24 条 N30A 请求全部保留并扩展。每一条请求与响应原样保存为 NDJSON。

## 9. 测试语料与磁盘

### 9.1 四类语料

1. **真实短 Trace**：优先使用现有 `N29-Autonomous-Smoke\Smoke-20260830-214952`，要求同时具备兼容的 stream 和 Traditional Full Worker `.tracy`。缺失时 P3 为 `BLOCKED_ENVIRONMENT`，Synthetic 不替代该门禁。
2. **真实 30 分钟 Trace**：执行全新冷转换，并对既有 A7 Session 做只读比较。
3. **小型故障语料**：用于取消、终止、损坏、身份和 lease。
4. **Synthetic**：分别验证两小时逻辑跨度和数据密度增长。

### 9.2 两小时 Synthetic

拆为两个正交测试：

- 时间跨度：严格覆盖两小时，使用 64 位 Frame ID，至少跨 240 个 30 秒 Shard，并注入跨 Shard Zone、Job、Allocation、Resource 和 I/O 生命周期。
- 数据密度：相同模型生成 `1x/2x/4x`，比较记录、磁盘、时间、内存和 Shard 增长。

报告固定说明：

```text
Executed_Logical2HourDuration
Executed_BoundedDensityScaling
NotExecuted_Combined2HourProductionDensity
Reason=DiskSafetyReserveAndSessionLimit
```

合并的两小时真实生产密度不参与验收 A 的 Pass/Fail，也不得写成已执行。

### 9.3 磁盘规则

2026-09-06 设计确认时 C 盘可用空间为 1,046.27 GiB。运行时以实时预检为准。

- 新冷转换 Session 和所有新增 scratch 合计硬预算 320 GiB。
- 始终保留 `max(64 GiB, volume capacity 10%)` 可用空间；当前卷 10% 约 381.50 GiB。
- 预估超限时不得启动对应阶段。
- 达到安全线时 checkpoint 并返回 `BLOCKED_ENVIRONMENT` 或明确资源状态。
- 不复制既有 188.253 GiB Session。
- 默认不自动删除任何输入、Session 或 scratch。

## 10. 故障与恢复

### 10.1 安全取消

在 Inventory、Canonical、Checkpoint、Derived、GPU Analysis 和 Final Audit 分别等待进入目标状态，然后发送一次 Windows Console Control Event。

要求：

- 两秒内响应。
- 状态为 `CancelledResumable`。
- 已提交 Shard checksum 正确。
- 未提交 `.tmp` 不进入 Manifest。
- `CURRENT` 不指向未完成 generation。
- Resume 后结果与不中断 Oracle 完全一致。

短时间内第二个取消信号只能提示安全收尾，不得强杀或破坏 checkpoint。

### 10.2 非安全终止

Python 只终止自己启动且重新核对以下身份的进程树：

```text
PID + creation time + executable absolute path
```

不得按进程名批量结束 `tracy-*`。在 payload、checkpoint 和 rename 前后终止后，恢复必须从最后有效 checkpoint 继续，最终不重复、不遗漏。

### 10.3 损坏与身份

小型副本覆盖：

- stream 尾部截断和 trailing bytes。
- committed revision 损坏。
- Shard payload 单字节翻转。
- checksum/header 损坏。
- Manifest 缺项和错误 `CURRENT`。
- Resume 时 source SHA 变化。
- Converter/Schema 与 building 目录不匹配。

必须得到明确的 `InvalidSource`、`InvalidChecksum`、`IdentityMismatch` 等结果；不得猜测、跳过或发布部分 generation。

### 10.4 Writer lease

- 第二 writer 必须被拒绝。
- 活跃 PID + creation time 不允许接管。
- stale lease 可恢复。
- PID 数值复用但 creation time 不同，不能误认原 writer。
- 短暂 heartbeat 延迟不能直接抢占。

### 10.5 内存和磁盘故障

不填满真实磁盘，也不故意耗尽机器内存。使用低预算测试配置触发同一生产限制代码，Windows Job Object 只作为外层紧急保护。

若需要确定性 fault hook，只允许测试构建启用；Release 工具不得暴露任意故障注入入口。

## 11. 性能测量

### 11.1 Converter

Python 每秒采集被测进程树：

- Working Set、Private Bytes、Commit。
- CPU time 和逻辑核心归一化利用率。
- Read/Write bytes 和吞吐量。
- Handle、线程和子进程。
- stage、source records/bytes、Shard、Canonical/Derived bytes、checkpoint。

阶段分别统计 Inventory、CapacityPreflight、Canonical、MandatoryDerived、GPU Analysis、FinalAudit、Publish 和 Total。

内存规则：

| 指标 | 判定 |
|---|---|
| 稳态目标 8 GiB | 超过则 warning，不能隐藏 |
| 软上限 12 GiB | 必须观察到停止预取、flush 等降压行为 |
| 硬上限 16 GiB | `FAIL_PERFORMANCE` |
| 外层紧急保护 18 GiB | Job Object 触发即严重失败 |

转换总墙钟时间只测量，不设武断上限；连续 120 秒无 source、Shard、checkpoint 或阶段进展且未声明 `Paused` 时失败。

### 11.2 Query

每个代表请求用全新 Query 进程执行三次，记录启动、Session 打开、请求、总耗时和峰值内存。中位数判定常规门槛，Cancel 使用最慢值。

| 操作 | 门槛 |
|---|---:|
| Manifest/摘要打开 | ≤2秒 |
| 单 Frame/Resource | ≤2秒 |
| 跨域关系 | ≤5秒 |
| Cancel | 三次均≤2秒 |
| Query 进程树内存 | ≤16 GiB |
| 单响应 | ≤16 MiB |

三次是预定重复测量，不是失败后的静默重试；每次原始结果均保留。

### 11.3 Scale

`1x/2x/4x` 保存输入记录、输入字节、Canonical/Derived/临时磁盘、时间、峰值/P95内存、Shard 数、records/s 和 MiB/s。

输入翻倍时核心构建时间不得超过 2.5 倍；峰值内存不得随累计事件量持续同比例增长；Shard 数允许线性增长。

### 11.4 验收器隔离

Python 自身 CPU 和内存单独记录，不计入 Converter/Query 门槛。重型阶段串行执行，不互相争抢资源。系统基本信息和外部高 I/O 进程只作为环境证据，不能自动用于忽略失败。

## 12. 输出、恢复与审计

### 12.1 Run 布局

```text
runs\<run-id>\
├─ environment.json
├─ tool-identities.json
├─ input-identities.json
├─ commands.ndjson
├─ progress.ndjson
├─ raw\
├─ metrics\
├─ comparisons\
└─ report\
```

每阶段原子写结果。每五秒更新验收根目录的 `acceptance-progress.json`，包含阶段、已运行时间、最新进度、资源、预计剩余和最后错误。

### 12.2 最终文件

```text
acceptance-result.json
acceptance-metrics.json
acceptance-comparison.json
acceptance-junit.xml
N30A-Full-Acceptance-Report.zh-CN.md
EVIDENCE-SHA256.txt
rerun-command.txt
```

Markdown 报告必须包含：

1. 最终结论和范围。
2. 延期项。
3. Git、工具、输入、Session 和配置身份。
4. 阶段结果。
5. 全域计数。
6. 新旧 Session 与 Golden 差异。
7. 源端退化。
8. GPU 全量关系和峰值。
9. Query 矩阵。
10. Cancel/Resume 和故障恢复。
11. Converter/Query 的时间、内存、CPU、I/O 和进度。
12. Synthetic 规模曲线。
13. 失败证据和明确修复方向。
14. 重现命令和证据路径。

机器结果是权威；Markdown 只能由机器结果确定性生成。

## 13. 安全约束

- 验收读取的正式 source 和既有 Session 均只读。
- 新冷转换写入独立验收目录。
- 故障注入只作用于验收器创建的 disposable 数据。
- 不通过 wildcard、进程名或宽路径删除/终止对象。
- 进程终止必须验证 PID、creation time 和 EXE 路径。
- 验收器不得修改 Golden 以使失败通过。
- 不自动清理输出。
- 不自动提交实现以外的文件，不自动合并或推送。
- 离线验收不需要 Windows 管理员权限。

## 14. 实现与测试约束

验收器实现使用 TDD：

1. 先为精确整数、Golden 绑定、排除字段、状态汇总和报告写失败测试。
2. 再为进程树、取消、超时、原子阶段结果和 Resume 写失败测试。
3. 用 fixture 测 Query JSON/NDJSON、分页和错误语义。
4. 先通过 Harness 自测，才允许调用真实 N30 工具。

正式工具从设计基线或经明确批准的新 N30 HEAD 重新构建到独立目录。验收从 P2 起锁定工具 SHA；中途源代码或二进制变化必须创建新的 Run。

发现产品 Bug 时：

```text
停止依赖阶段
→ 保存最小复现、原始响应和差异
→ 生成 FAILED 报告
→ 另立修复任务
```

验收流程本身不进入自动修复—重建—重跑循环。

## 15. 完成定义

只有以下条件全部满足，验收 A 才能返回 `PASSED`：

1. Harness 自测通过。
2. 当前 N30 工具构建和既有单元测试通过。
3. 真实短 Trace 的 Full Worker 与 Session 差分通过。
4. 固定 30 分钟 source 从零生成新 Session 并发布。
5. 新 Session、既有 A7 Session 和 Golden 的所有 Required 指标零误差。
6. Inventory、Canonical、Derived 的 source count 守恒。
7. GPU Verifier 的 count、bytes、peak、checksum、正反关系和 report hash 全部一致。
8. Converter-created unresolved、Core gap 和 checksum failure 为零。
9. 已知 source degradation 被完整、准确保留。
10. Query 全域矩阵、分页、范围、跨域和取消通过。
11. 故障注入、Resume、identity 和 writer lease 通过。
12. Converter 硬内存不超过 16 GiB，Query 不超过 16 GiB。
13. 两小时逻辑跨度和 1x/2x/4x 有界密度测试通过。
14. 原始 stream 和既有 Session 身份在验收后不变。
15. 机器结果、JUnit、Markdown 和证据哈希完整。

局部 `.tracy` 导出和 Profiler 直接 Session 后端保持延期，不影响本轮验收 A，但必须清楚显示为 `DEFERRED_OUT_OF_SCOPE`。

## 16. 提交与交付边界

设计文档、验收脚本、fixture、测试、Profile 和 Golden 提交到：

```text
feature/JNTracy-N30-LongTraceSessionStore
```

不合并、不推送。大型 Session、stream、raw response、metrics 和报告只保留于本地验收目录。完成后另行列出安全清理候选，不自动执行清理。
