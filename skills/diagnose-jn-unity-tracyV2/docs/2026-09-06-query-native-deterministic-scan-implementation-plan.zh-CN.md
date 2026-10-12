# JN Unity × Tracy 单 Trace AI 性能分析工作流实施计划

## Query 原生确定性扫描版

状态：等待用户评审  
设计规格：[2026-09-06-query-native-deterministic-scan-design.zh-CN.md](2026-09-06-query-native-deterministic-scan-design.zh-CN.md)  
目标 Skill：`diagnose-jn-unity-tracy`  
计划日期：2026-09-06

## 1. 实施目标

将当前由PowerShell、Python和AI共同完成的全量Trace扫描，迁移为Tracy Query内部的确定性扫描服务：

```text
.tracy / N30 Session
→ Query Native Scanner
→ Neutral Aggregate
→ Policy Evaluation
→ Candidate Manifest
→ AI MCP Deep Dive
→ 可审计报告
```

完成后：

- Query扫描所有有效帧和所有已启用数据域。
- Python不再扫描Trace、Zone、Job、GPU Pass或Allocation。
- 修改预算和候选阈值不重新扫描Trace。
- AI只调查Query产生的候选，并通过MCP回查具体事件。
- 报告包含结论、完整分析过程、反证、限制和复测方向。
- `.tracy` 与N30 Session共用同一统计与候选语义。

## 2. 基线与隔离

### 2.1 Tracy基线

固定从以下基线创建新分支：

```text
仓库：
C:\CodeProjects\GodotProjects\tracy-0.13.1

源分支：
feature/JNTracy-N30-LongTraceSessionStore

源HEAD：
3fc8b3d9d4df304de4ebf043395bc132a1cc83a4

新分支：
feature/JNTracy-AI-QueryScan

新worktree：
C:\CodeProjects\GodotProjects\tracy-0.13.1-ai-query-scan
```

执行前必须验证：

- 源分支仍包含 `3fc8b3d9`。
- 新分支和worktree尚不存在，或与上述基线完全一致。
- N30 worktree现有未跟踪 `build-n30-stream-tests` 不移动、不删除。
- 不切换、不修改、不提交 `dev`、N29或N30工作树。
- N30分支当前ahead状态保持不变。

### 2.2 Skill草案

新版Skill仅在以下目录开发：

```text
C:\Users\Admin\Documents\JN-Unity-T3\skill-draft\diagnose-jn-unity-tracy
```

验收通过前不得覆盖：

```text
C:\Users\Admin\.codex\skills\diagnose-jn-unity-tracy
```

当前工作区Git尚无正式提交且存在大量无关文件，因此Skill草案不在该仓库提交。每个阶段记录：

- 修改文件列表。
- 修改前后SHA-256。
- 备份位置。
- 对应Tracy提交。
- 测试结果。

### 2.3 不修改范围

本计划不修改：

- `C:\workflow\jnunity`
- `C:\workflow\PackageRepo`
- Unity Player包体。
- Tracy录制Protocol 90。
- JN trace section 12。
- Unity采集INI。

如果实现过程中确认必须修改上述路径，立即暂停并重新与用户确认。

## 3. 版本和Schema

| 项目 | 基线 | 目标 |
|---|---:|---:|
| Tracy录制Protocol | 90 | 保持90 |
| JN trace section | 12 | 保持12 |
| N30 Session schema | 1 | 保持1 |
| Query API | 1.34.0 | 1.35.0 |
| Native Scan API | 无 | 1 |
| Neutral Aggregate schema | 无 | 1 |
| Policy Evaluation schema | 无 | 1 |
| Candidate Manifest schema | 无 | 1 |
| Analysis Profile schema | 旧JSON组合 | YAML schema 1 |
| Skill | 1.1.2 | 2.0.0 |
| Analysis State | 3 | 4 |
| Analysis Result | 现有 | 2 |

兼容规则：

- Query 1.35保留Query 1.34全部已有方法和响应语义。
- 旧`.tracy`和完成的N30 Session继续可打开。
- 缺少某数据域时返回 `present=false`，不制造空健康结论。
- 已有N29/N30 GPU Analysis结果继续作为GPU事实来源。
- Scan schema不改变Trace或Session格式。
- Skill 2.0不继续旧Schema 3的中断任务；旧报告保持只读。

## 4. 目标文件布局

### 4.1 Tracy worktree

计划新增：

```text
analysis/
├─ TracyDeterministicScanTypes.hpp
├─ TracyAnalysisProfile.hpp
├─ TracyAnalysisProfile.cpp
├─ TracyNeutralAggregate.hpp
├─ TracyNeutralAggregate.cpp
├─ TracyNeutralAggregateStore.hpp
├─ TracyNeutralAggregateStore.cpp
├─ TracyZoneTreeScanner.hpp
├─ TracyZoneTreeScanner.cpp
├─ TracyDomainScanners.hpp
├─ TracyDomainScanners.cpp
├─ TracyExactStatistics.hpp
├─ TracyExactStatistics.cpp
├─ TracyCandidatePolicy.hpp
├─ TracyCandidatePolicy.cpp
├─ TracyScanArtifacts.hpp
└─ TracyScanArtifacts.cpp

query/src/
├─ TracyAnalysisScanManager.hpp
├─ TracyAnalysisScanManager.cpp
├─ TracyAnalysisScanApi.hpp
└─ TracyAnalysisScanApi.cpp

query/schema/
├─ analysis-profile-v1.schema.json
├─ neutral-aggregate-v1.schema.json
├─ candidate-manifest-v1.schema.json
└─ analysis-scan-api-v1.schema.json

query/tests/
├─ DeterministicScanTests.cpp
├─ AnalysisProfileTests.cpp
├─ AnalysisScanManagerTests.cpp
├─ AnalysisScanMcpTests.cpp
└─ fixtures/analysis-scan/

doc/
└─ JN-Tracy-AI-Query-Native-Scan-Implementation-Plan.zh-CN.md
```

计划修改：

```text
analysis/TracyTraceSource.hpp
analysis/TracyWorkerTraceSource.hpp/.cpp
query/src/TracySegmentTraceSource.hpp/.cpp
query/src/TracyQueryService.hpp/.cpp
query/src/TracyMcpServer.hpp/.cpp
query/src/main.cpp
query/schema/tracy-query-v1.schema.json
query/schema/coverage-v1.json
query/schema/coverage-fields-v1.json
query/schema/coverage-mcp-v1.json
query/tests/ProtocolTests.cpp
query/tests/McpTranscriptTests.cpp
query/tests/TraceSessionInventoryTests.cpp
query/CMakeLists.txt
cmake/analysis.cmake
```

`TracyQueryService.cpp`已经很大，新扫描算法不得继续堆进其单一translation unit。该文件只保留参数解析和对 `AnalysisScanApi` 的分派。

### 4.2 Skill草案

计划新增或重写：

```text
SKILL.md
config/JNTracy.AnalysisProfile.example.yaml
config/JNTracy.AnalysisProfile.yaml
config/analysis-profile.schema.json  # 从Query权威Schema生成的安装镜像
assets/analysis-state-template.json
references/query-native-scan-workflow.md
references/candidate-investigation.md
references/domain-playbooks.md
references/evidence-and-source.md
references/report-contract.md
scripts/resolve-analysis-profile.py
scripts/run-query-scan.ps1
scripts/validate-scan-output.py
scripts/build_report.py
schemas/analysis-result.schema.json
schemas/evidence-manifest.schema.json
schemas/report-manifest.schema.json
tests/test-profile-resolution.py
tests/test-scan-contract.py
tests/test_report_builder.py
```

保留现有capture/start/stop/convert脚本。旧的全量Frame扫描和候选生成脚本从正式 `SKILL.md` 路由中移除；验收结束后再决定删除还是移入 `legacy/`。

Analysis Profile Schema只在Tracy Query仓库维护一份权威源文件。Skill内副本在构建或安装时生成，并通过SHA-256测试保证与Query嵌入Schema一致，禁止人工维护两份不同定义。

## 5. 实施阶段

## QSCAN-0：创建隔离分支、记录Oracle和回退点

### 操作

1. 验证N30基线和worktree状态。
2. 从 `3fc8b3d9` 创建 `feature/JNTracy-AI-QueryScan`。
3. 创建独立worktree。
4. 在新worktree建立 `build-query-scan`。
5. 将已批准实施计划复制到新worktree的 `doc` 目录。
6. 记录Query 1.34、N30测试、工具SHA-256和当前MCP capability。
7. 完整复制当前Skill 1.1.2到带时间戳的只读备份目录。
8. 保存当前Skill在两份真实Trace上的报告和Query响应作为行为基线，不把旧报告结论当正确性Oracle。

### 固定真实输入

```text
GPU Catalog关闭：
C:\Users\Admin\Documents\JN-Unity-T3\PlayerCaptures\Admin-HighEvidence-Manual\GPUCatalogOff-20260903-202313\GPUCatalogOff-HighEvidence-20260903-202313.tracy

HighEvidence全域：
C:\Users\Admin\Documents\JN-Unity-T3\PlayerCaptures\Admin-HighEvidence-Manual\G05-Player-HighEvidence-20260831-173806\G05-Player-HighEvidence-20260831-173806.tracy

30分钟原始stream身份参考：
C:\Users\Admin\Documents\JN-Unity-T3\N29-Autonomous-30m\Run-20260830-220012\Admin-HighEvidence-30m.tracy-stream
SHA-256 2AC46C53257CE9027EC67E098FC15070FB911243F6CD311A166EB97547848068

30分钟已发布N30 Session：
C:\Users\Admin\Documents\JN-Unity-T3\N29-Autonomous-30m\Run-20260830-220012\Admin-HighEvidence-30m.jn-trace-session
```

N30长Session只使用上述目录中 `CURRENT` 指向、已经完整发布且通过Audit的generation；不得由Scan测试读取 `.building` 目录。

### 门禁

- 新worktree基线精确为 `3fc8b3d9`。
- 原N30和dev工作树状态不变。
- Query 1.34现有CTest全部通过。
- 当前Skill备份有manifest和SHA-256。

### 提交

```text
QSCAN-0 Record native scan baseline and oracle inventory
```

## QSCAN-1：先建立契约测试和版本骨架

### 测试先行

先添加失败测试：

- Query schema必须为1.35.0。
- 新方法全部出现在Method Registry、operation schema和MCP coverage中。
- 工具注解为只读、非破坏、幂等。
- 请求未知字段、错误类型和超限响应必须失败。
- 1.34旧方法的contract快照不发生非预期变化。
- Scan输出必须携带Trace、Aggregate、Policy和Schema identity。

### 实现

1. 定义Scan、Aggregate、Policy和Candidate POD/DTO。
2. 增加Schema文件并嵌入Query构建。
3. `QuerySchemaVersion`升级到1.35.0。
4. 增加方法注册，不实现扫描时返回明确 `NOT_IMPLEMENTED_SCAN_SCHEMA_1`。
5. 更新CLI `--version`、doctor和system.describe。

### 门禁

- `tracy-query-contract-tests`通过。
- `tracy-query-mcp-transcript-tests`通过。
- 旧方法列表、字段和分页行为差分通过。
- 不改变Protocol 90和Session schema。

### 提交

```text
QSCAN-1 Add Query 1.35 deterministic scan contracts
```

## QSCAN-2：Analysis Profile和安全路径

### 测试先行

- 完整Profile通过。
- 缺字段、未知字段、单位错误、负预算和错误scope失败。
- 6.4 GB按十进制解析为6,400,000,000 bytes。
- 16 GB预算保留Provisional状态。
- 未定义GPU Pass预算不能变为Budget Candidate。
- 路径逃逸analysis root和cache root失败。
- 同一标准化Profile生成同一hash。
- 键顺序、注释和无意义空白不改变hash。

### 实现

1. Query新增严格 `AnalysisProfile` JSON验证和标准化。
2. `analysis.scan.profile.validate` 接收解析后的JSON对象，返回规范化Profile及SHA-256。
3. Query启动参数增加：
   - `--analysis-root`
   - `--analysis-cache-root`
4. 所有写入路径必须位于上述允许根目录。
5. Skill提供YAML authoring文件。
6. `resolve-analysis-profile.py` 只使用安全YAML loader解析配置并传给Query，不读取Trace、不做统计。
7. Skill内vendor PyYAML 6.0.2的纯Python安全解析路径及MIT许可证，禁用可选C扩展；运行时不联网安装依赖。
8. Query返回的规范化JSON重新写为 `resolved-analysis-profile.yaml` 和JSON审计副本。

### 安全要求

- 禁止YAML对象构造、自定义tag和代码执行。
- 禁止使用Trace字符串生成路径。
- 绝对路径必须来自Profile。
- 工具和源码路径存在性分别验证，缺失时按功能域阻断。

### 门禁

- Profile测试全部通过。
- YAML与规范化JSON往返结果一致。
- 当前预算和Marker规则成功迁移。
- Skill中不再存在第二份可执行预算。

### 提交

```text
QSCAN-2 Add strict external analysis profile contract
```

## QSCAN-3：Neutral Aggregate存储、身份和原子发布

### 测试先行

- Manifest、run、dictionary和checkpoint round-trip。
- Payload/header/checksum损坏必须拒绝。
- 中断前已提交run保持可读。
- 未提交临时文件不进入manifest。
- 不同Trace、算法或schema不能复用。
- 只改Policy Profile可以复用Neutral Aggregate。
- active generation不被cache清理。

### 实现

目录：

```text
<analysis-cache-root>\
└─ <trace-strong-id>\
   └─ native-scan-v1\
      ├─ aggregate-manifest.json
      ├─ dictionaries\
      ├─ domains\
      ├─ signature-frame-runs\
      ├─ checkpoints\
      └─ audit\
```

1. 实现强身份、算法身份、Schema身份和config-independent Aggregate identity。
2. 使用immutable run、SHA-256和原子rename。
3. 建立writer lease、PID、process start time和heartbeat。
4. 支持cancel checkpoint、resume和stale lease恢复。
5. 记录阶段、输入事件、输出run、内存、磁盘和进度。
6. 实现LRU cache清理，但不删除active、pinned或报告引用的generation。

### 默认资源门槛

```text
Query进程目标内存： 8 GiB
Query进程硬上限：  16 GiB
Scan缓存默认上限：128 GiB
磁盘最低保留：    64 GiB
单MCP响应：沿用现有8 MiB硬上限
状态/取消响应目标：≤2秒
```

达到限制时保存安全检查点并返回 `RESOURCE_LIMIT_RESUMABLE`，禁止丢弃事件或使用近似统计。

### 门禁

- 故障注入后可恢复且最终SHA一致。
- 路径和identity错误不会读取旧缓存。
- Query总内存硬上限有效。
- 扫描120秒内始终有进度或明确暂停原因。

### 提交

```text
QSCAN-3 Add atomic neutral aggregate store and resume state
```

## QSCAN-4：为Worker与Session建立有界扫描源

### 背景

现有 `TraceSource` 部分接口会返回完整vector，例如Job、Relation或Frame Duration。它们不能作为长Session全域扫描的基础。

### 测试先行

- FakeTraceSource记录每个接口调用次数和最大批次。
- Session扫描不得调用明确标记为full-materialization的旧接口。
- Worker和Session对同一Synthetic事实返回相同顺序、字段和开放边界。
- 游标恢复不能重复或跳过实体。

### 实现

1. 为TraceSource增加有界scan cursor或visitor接口。
2. 覆盖Frame、CPU/GPU Zone、Job、Relation、Memory、Sampling、Context Switch、Script、I/O、Gfx和GPU Catalog。
3. WorkerTraceSource使用已有Worker容器，但每次只返回有界批次。
4. Segment/SessionTraceSource直接使用N30磁盘索引和Shard reader。
5. Scan cursor包含domain、time、ordinal、last stable entity和resume token。
6. 旧全量接口保持兼容，但Native Scanner禁止调用。
7. 增加测试断言，防止未来回退到full vector。

### 门禁

- Worker与Session事实差分100%一致。
- Session额外内存不随总事件量线性增长。
- 任意cursor边界无重复、遗漏和排序漂移。
- `.building` Session明确拒绝。

### 提交

```text
QSCAN-4 Add bounded Worker and Session scan cursors
```

## QSCAN-5：Frame与CPU Zone树扫描器

### 测试先行

Synthetic覆盖：

- 0～10层嵌套。
- 同名不同父路径。
- sibling重叠和区间union。
- 跨scan chunk的父子Zone。
- 缺Begin、缺End、负区间和clock inversion。
- Main、Render和Worker线程。
- 完整帧、捕获头尾帧和未归属Zone。
- `per_complete_frame` 与 `when_present` 双分母。

### 实现

1. 全量读取FrameSet和完整帧边界。
2. 为每个CPU timeline维护跨批次开放Zone stack。
3. 计算inclusive和direct child interval union。
4. 计算exclusive，不合法结构进入quality，不静默clamp。
5. 生成CPU结构化精确签名和Logical Rollup。
6. 输出SignatureFrameRun和代表event refs。
7. 分开处理Active Work、Wait和Intentional Pacing候选事实。

### 门禁

- inclusive/exclusive与独立brute-force Oracle 100%一致。
- 全深度Zone均被扫描。
- 父子候选可合并且不重复归因。
- 不完整边界不会支持否定性结论。

### 提交

```text
QSCAN-5 Add exact frame and CPU zone tree scanner
```

## QSCAN-6：GPU Zone、Job、Managed和关系扫描器

### 测试先行

- Direct/Compute/Copy真实Queue分离。
- L0 Segment数量多于Frame数量。
- 有一致Pass FrameId、无FrameId和时间相邻三种情况。
- Job Schedule→Ready→Slice→Complete→Wait。
- 无waiter、handle reuse和capture boundary。
- C#/Lua Source Stack存在和缺失。
- typed relation存在、缺失和歧义。

### 实现

1. GPU Queue内执行同样的全深度inclusive/exclusive扫描。
2. 生成GPU Pass精确签名和taxonomy rollup。
3. 实现GPU Track A与Track B双轨候选事实。
4. GPU范围分级为Exact Frame Relation、InferredFromChildPasses、TemporalCandidate和Unassigned。
5. 不根据L0相邻性生成确定性GPU帧。
6. Job按类型、stage、schedule callsite和线程类别聚合。
7. Managed/Lua按准确Source Stack聚合；UnityMarker callback与direct source区分provenance。
8. 建立候选可用的typed relation adjacency，不把时间共现写成exact relation。

### 门禁

- Queue内统计100%一致。
- GPU帧推断级别准确。
- 无waiter Job不产生虚假返回。
- C#/Lua无栈时仍保留时间事实并返回明确原因。

### 提交

```text
QSCAN-6 Add GPU Job managed and relation scanners
```

## QSCAN-7：Memory、I/O、Sampling与Telemetry扫描器

### 测试先行

- CPU allocation创建/释放、地址复用和open boundary。
- GPU physical、logical、owner、reference、range和alias。
- GPU Catalog Disabled、Invalid和Absent。
- I/O request完整、取消、错误和orphan。
- Sampling与Context Switch可用和权限不可用。
- Tracy producer、worker、FrameImage、写盘和drop质量事实。

### 实现

1. CPU Memory分类Capacity、Growth、Churn、LeakCandidate、Transient和AccountingGap。
2. GPU Memory严格分离DXGI、physical、resident、owned、working set、range和untracked。
3. GPU Catalog invalid时阻止资源级来源，但不污染独立物理总量。
4. I/O按完整生命周期和阶段聚合。
5. Sampling按标准化栈和叶函数聚合。
6. Context Switch按线程角色、运行、等待和抢占聚合。
7. 建立独立TelemetryCost域。
8. Telemetry候选区分MeasuredSelfCost、ObservedSystemPressure、EstimatedAttribution和RequiresABValidation。

### 门禁

- shared allocation不重复计入physical总量。
- referenced working set不冒充owned/physical。
- 短时增长不自动判为leak。
- 单Trace固定返回 `TotalCaptureOverhead=NotMeasuredSingleTrace`。
- 权限不可用不被解释为无Sampling/Context Switch问题。

### 提交

```text
QSCAN-7 Add memory IO sampling and telemetry scanners
```

## QSCAN-8：精确统计、异常和Neutral Audit

### 测试先行

- 偶数和奇数样本的精确百分位。
- 稀疏签名未出现帧计0与when-present分布。
- MAD为0、重复值、单峰、多峰、Burst和周期。
- 输入顺序变化不改变输出。
- 中断恢复与完整运行SHA一致。
- 大型数据使用external merge，不超过内存门槛。

### 实现

1. 合并SignatureFrameRun，得到每签名每帧精确聚合。
2. 通过external sort计算精确p50/p90/p95/p99/max。
3. 计算median、MAD、delta和异常集合。
4. 分类IsolatedSpike、RecurrentSpike、BurstWindow和PersistentPressure。
5. 计算域内exclusive/dedup累计贡献率。
6. 生成三种Top：Exclusive、Inclusive、Wait/Critical Path。
7. 完成全域count、checksum、frame coverage和quality audit。
8. 原子发布Neutral Aggregate。

### 门禁

- 统计与独立Oracle 100%一致。
- 不使用近似Sketch。
- 输入规模翻倍时核心扫描时间目标≤2.5倍。
- 无未报告事件、签名或Frame gap。

### 提交

```text
QSCAN-8 Add exact statistics anomaly detection and neutral audit
```

## QSCAN-9：Policy Evaluation与Candidate Manifest

### 测试先行

- Budget、Top、Anomaly、Capacity、Quality和UserFocus独立触发。
- 一个问题多入口命中后只产生一个Candidate Family。
- P0～P4优先级确定性一致。
- 模块预算scope不匹配时不触发。
- 未定义预算不产生Budget Candidate。
- Top 10、80%扩展和每域50上限正确。
- 代表帧覆盖worst、typical、first/last、burst和结构变化。

### 实现

1. 从规范化Profile建立Policy identity。
2. 运行六个候选池。
3. 按结构签名和关系合并Candidate Family。
4. 生成P0～P4，不计算混合加权总分。
5. 选择代表帧和event refs。
6. 输出全部ranked signatures和候选。
7. 保存未入选原因，支持漏选审计。
8. 修改Profile时只重新执行本阶段。

### 门禁

- 同输入输出SHA一致。
- Query数值与Candidate字段一致。
- 不因候选上限隐藏Investigation Backlog。
- Profile变化不触发Trace重扫。

### 提交

```text
QSCAN-9 Add deterministic candidate policy evaluation
```

## QSCAN-10：持久异步Scan Manager与MCP接口

### 背景

当前MCP异步Job只保存在进程内，只有queued/running/cancelled状态；QueryService还使用全局 `m_queryMutex`。新Scan不能长时间占用该mutex，否则status以外的普通Query会被阻塞。

### 测试先行

- start立即返回scan_id。
- status在扫描期间可调用且≤2秒。
- cancel在≤2秒内被观察并进入安全收尾。
- Query进程重启后发现并resume已有scan。
- 相同identity并发start只保留一个writer。
- 完成后summary/signature/candidate分页可查。
- 扫描期间普通轻量MCP查询不被全局mutex阻塞。

### 实现

1. `AnalysisScanManager`独立拥有worker、lease和持久状态。
2. Scan worker直接取得不可变 `TraceReadView` 或有界Source cursor，不通过长时间持锁的 `QueryService::Execute`。
3. QueryService只负责短请求分派和Scan Manager控制。
4. 复用现有MCP工具风格，但Scan状态不依赖易失的 `McpServer::Job` map。
5. 增加专用 `tracy_scan` MCP工具和通用 `tracy_query` 方法访问。
6. `analysis.scan.signatures`、`candidates`和`candidate.get`强制分页/投影。
7. 每个响应继续受8 MiB硬上限控制。
8. 支持analysis.scan.close释放pin，但不删除持久结果。

### 门禁

- 取消、恢复、重复start和Query崩溃测试通过。
- status/progress可靠。
- 扫描不会阻塞trace.status、system.describe和候选深查。
- MCP method registry与schema一致。

### 提交

```text
QSCAN-10 Add persistent asynchronous scan manager and MCP API
```

## QSCAN-11：重写Skill流程和状态机

### 前置技能

开始修改Skill前必须完整读取并遵循：

- `writing-skills`
- `skill-creator`
- `agent-skill-linter`

需要端到端质量评估时再使用 `skill-evaluator`。

### 测试先行

- Skill触发描述只匹配单Trace JN Unity Tracy诊断。
- Profile缺失或不合法时停止。
- Query schema不是1.35时停止新流程。
- 已有Aggregate复用正确。
- CancelledResumable能够恢复。
- P0/P1和UserFocus全部进入调查队列。
- 数值字段被AI改写时报告校验失败。

### 实现

1. Skill版本升级到2.0.0。
2. `SKILL.md`改为Query原生扫描的可恢复编排。
3. Analysis State升级为Schema 4。
4. YAML Profile替换四份分散运行时JSON权威。
5. Skill启动唯一常驻Query MCP。
6. 通过 `analysis.scan.start/status` 等待Neutral Aggregate和Policy完成。
7. 读取分页Candidate Manifest，不加载全量raw events。
8. 按覆盖规则建立AI调查队列。
9. 对每个候选保存假设、MCP Evidence、源码核查和结果。
10. 所有未调查候选进入Investigation Backlog。
11. 现有录制、停止和转换能力保持独立路由。

### 正式流程中禁止

- Python全量扫描。
- AI自己计算percentile或exclusive time。
- 通过固定12帧替代全量扫描。
- 一次性MCP CLI旁路。
- 把partial、truncated或失败解释为空结果。

### 门禁

- Skill linter通过。
- 触发/不触发测试通过。
- 中断恢复不重复已完成候选。
- 所有MCP响应和源码证据有ledger。

## QSCAN-12：报告Schema、生成器与分析过程

### 测试先行

- 缺少候选触发路径的重大结论失败。
- 缺少代表帧选择理由失败。
- AI数值与Candidate Manifest不一致失败。
- Confirmed缺少精确关系或源码/独立证据失败。
- Source revision mismatch不能生成Confirmed源码结论。
- Tracy单Trace报告不能声称总体A/B开销。
- 相同输入两次报告SHA一致。

### 实现

1. Analysis Result升级到Schema 2。
2. 每个问题增加Analysis Trail：发现、统计、代表帧、逐层归因、假设、源码、反证和结论。
3. 生成固定主报告和9份专项报告。
4. Query聚合结果、Candidate、MCP Evidence和源码证据分别登记。
5. `07-Tracy-Telemetry-Overhead.md` 独立生成。
6. `08-Profiler-Manual-Verification-Guide.md` 给出帧号、时间、线程、Queue、Zone和资源导航步骤。
7. 主报告保留完整分析过程，但不输出模型内部不可审计思考文本。
8. 报告继续使用确定性Renderer；Renderer不得重新统计Trace数据。

### 门禁

- 所有P0/P1/UserFocus有完整分析路径。
- 主报告易读且专项证据可追溯。
- 报告链接、事件ID、帧号和源码路径有效。
- 两次确定性构建一致。

## QSCAN-13：集中正确性、性能和人工验收

### 13.1 Synthetic

执行所有Query和Skill单元测试，重点覆盖：

- CPU/GPU树和exclusive union。
- Job/Wait和关系。
- 稀疏序列、精确分位数和异常。
- Memory/IO/Sampling/Telemetry。
- invalid/disabled/absent。
- 取消、恢复、身份和缓存。

### 13.2 Worker `.tracy`

分别分析GPU Catalog关闭和HighEvidence全域Trace：

```text
原始Query事实
↔ Native Neutral Aggregate
↔ Candidate Manifest
↔ AI报告
↔ Profiler人工观察
```

要求：

- Frame、Zone、Job、Memory和GPU事实计数一致。
- Query tree与Profiler选定Zone的父子路径一致。
- 自动Top与GUI差异必须由明确的exclusive/inclusive、scope或分母差异解释。
- 不能再次出现“AI结论与GUI完全不同但报告不披露口径”的情况。

### 13.3 N30 Session

- 直接以完成的Session generation为Trace输入。
- 不构造完整Worker。
- 扫描全部有效数据域。
- 与同源短窗口导出的`.tracy`做逐域差分。
- 长Session过程中MCP状态、取消和恢复有效。

### 13.4 性能门槛

| 项目 | 门槛 |
|---|---:|
| Query总内存 | 目标≤8 GiB，硬上限16 GiB |
| 单MCP响应 | ≤8 MiB |
| status/cancel | ≤2秒 |
| 相同Aggregate的Policy重算 | 目标≤5秒 |
| 已完成摘要打开 | ≤2秒 |
| 单Candidate定向读取 | ≤2秒 |
| 跨域Candidate Evidence | ≤5秒 |
| 输入规模翻倍 | 核心扫描时间≤2.5倍 |
| Exact统计一致性 | 100% |
| 未报告Scan gap | 0 |
| 中断恢复结果一致性 | 100% |

首次全量扫描墙钟时间作为测量项记录，不以牺牲正确性设定虚假上限。

### 13.5 报告验收

- P0/P1和用户关注项调查完整。
- Raw与Robust统计同时存在。
- 扫描覆盖率、调查覆盖率和未覆盖贡献率明确。
- GPU L0归帧限制明确。
- Tracy成本与A/B反事实边界明确。
- 每项优化方向有风险、理论收益边界和复测指标。

### 提交

```text
QSCAN-13 Complete native scan and AI diagnosis acceptance
```

## QSCAN-14：发布同名Skill和收尾

该阶段只有在用户看过验收报告并明确批准后执行。

### 操作

1. 对已安装Skill 1.1.2建立最终归档快照。
2. 将草案原位安装为 `diagnose-jn-unity-tracy`。
3. 验证Codex Skill发现和描述触发。
4. 执行一次安装目录端到端smoke。
5. 保存Skill文件清单和SHA-256。
6. Tracy功能分支保留，不自动合并或推送。

### 回退

- 删除失败安装目录前先核对目标绝对路径。
- 从归档快照恢复1.1.2。
- Query 1.35独立build目录不覆盖 `build-latest`，直到用户批准切换。
- 不使用 `git reset --hard`。

### 完成门禁

- 同名Skill可正常触发。
- 旧Skill可完整恢复。
- 已安装版本和Query build身份写入报告。
- 用户确认后再讨论合入N30或dev。

## 6. 主要构建和验证次数

开发中会有小型增量单元编译，但主要构建限定为四个里程碑：

| 里程碑 | 内容 |
|---|---|
| Build 1 | QSCAN-1～4：Query契约、Store和Source cursor |
| Build 2 | QSCAN-5～8：全部Domain Scanner和Neutral Aggregate |
| Build 3 | QSCAN-9～10：Policy、Async Manager和MCP |
| Build 4 | QSCAN-11～14：最终Query、Skill和真实Trace验收 |

不需要重新编译Unity Editor、Player或JNTracyClient。

## 7. 阶段性验收策略

为避免每阶段重复进行重型真实Trace分析：

- QSCAN-1～4只运行契约、单元和小型Session测试。
- QSCAN-5～10每阶段运行对应Synthetic与一个小型Worker/Session fixture。
- QSCAN-11～12只使用已经完成的Aggregate和候选fixture测试Skill/report。
- QSCAN-13才进行两份真实`.tracy`和长N30 Session的集中验收。
- 任一正确性门禁失败时停止进入下一阶段。
- 性能数值未达到目标时记录并优化，但不得通过近似、采样或丢数据绕过。

## 8. 风险与控制

### 8.1 QueryService全局锁

风险：长扫描经过现有 `m_queryMutex` 会阻塞全部查询。  
控制：Scan Manager绕过长时间 `QueryService::Execute`，直接使用不可变ReadView/有界cursor；QueryService只做短控制请求。

### 8.2 TraceSource全量vector

风险：Session扫描意外构建完整Job/Relation/Zone vector。  
控制：新增有界cursor，并在FakeTraceSource测试中禁止Scanner调用旧全量接口。

### 8.3 Profile解析依赖

风险：本机没有PyYAML或PowerShell YAML模块。  
控制：Skill vendor固定、安全的纯Python YAML parser和许可证；不在运行时pip install，不允许自定义tag。

### 8.4 精确统计磁盘增长

风险：精确稀疏序列仍可能较大。  
控制：压缩immutable run、64位稀疏键、external merge、预检磁盘和128 GiB缓存预算；不退化为近似结果。

### 8.5 签名高基数

风险：动态名称、对象ID或调用栈变化导致签名爆炸。  
控制：结构化签名排除动态ID；名称标准化只接受已配置规则；异常高基数作为Data Quality候选。

### 8.6 AI重新解释数值

风险：报告人工计算造成与Query不一致。  
控制：Analysis Result引用Candidate字段，Validator逐字段核对；AI只能增加文字判断和Evidence引用。

### 8.7 GUI口径不同

风险：Profiler inclusive排序与Query exclusive排序看起来矛盾。  
控制：报告同时给出三种排名、scope和分母，并提供固定人工导航步骤。

### 8.8 源码版本错位

风险：用当前源码解释旧Trace。  
控制：强制revision匹配；不匹配时限制为Hypothesis。

## 9. 预估工期

这是Query新分析子系统，不是简单修改Skill文案。按一个主执行者估算：

| 阶段 | 预估 |
|---|---:|
| QSCAN-0～3 | 3～5个工作日 |
| QSCAN-4～6 | 5～8个工作日 |
| QSCAN-7～10 | 6～9个工作日 |
| QSCAN-11～12 | 3～5个工作日 |
| QSCAN-13～14 | 3～5个工作日 |
| 合计 | 20～32个工作日 |

首个可用的Frame/CPU/GPU/Job扫描里程碑预计8～13个工作日；完整全域和正式Skill需要完成全部阶段。

## 10. 实施完成定义

只有以下条件全部满足才算完成：

1. 新开发位于独立分支和worktree，未污染N30/dev。
2. Query 1.35直接扫描`.tracy`和N30 Session。
3. Python不参与正式Trace扫描和候选生成。
4. Neutral Aggregate与Policy Evaluation完全分离。
5. 全深度CPU/GPU树和exclusive/inclusive正确。
6. 所有有效域完成确定性扫描或返回明确不可用状态。
7. 精确稀疏序列、分位数、MAD、Burst和Top结果正确。
8. Profile变化只触发Policy重算。
9. Scan异步、可取消、可恢复、可审计。
10. 全部聚合签名和候选均可落盘审计。
11. AI深查范围、停止条件和Backlog明确。
12. 源码结论验证revision。
13. GPU帧边界限制不被伪装。
14. Tracy单Trace开销措辞正确。
15. 报告包含完整分析路径而非简略结论。
16. Synthetic、两份真实Trace和长Session验收通过。
17. 新Skill通过linter和evaluator。
18. 用户批准后才覆盖已安装同名Skill。
19. 不自动合并或推送Tracy功能分支。
