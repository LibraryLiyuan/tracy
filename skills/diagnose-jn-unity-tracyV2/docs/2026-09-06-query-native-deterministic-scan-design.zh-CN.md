# JN Unity × Tracy 单 Trace AI 性能分析工作流重构设计

## Query 原生确定性扫描版

状态：设计已确认，等待用户评审书面规格后制定实施计划  
日期：2026-09-06  
目标 Skill：`diagnose-jn-unity-tracy`  
当前已安装版本：`1.1.2`

## 1. 设计目标

本次重构用于解决当前单 Trace 性能分析工作流中的三个根本问题：

1. 大量 Trace 事实经 MCP 分页传给智能体或 Python 后再次扫描，数据规模、上下文和运行时间不可控。
2. Query、Python 和报告逻辑分别解释 Zone 层级、统计口径和数据质量，容易发生语义漂移。
3. 预算、Top 排名和异常检测混合在报告阶段，曾出现自动结论与 Tracy Profiler 人工观察不一致的问题。

重构后的正式数据流为：

```text
.tracy / N30 Trace Session
          ↓
常驻 Tracy Query MCP
          ↓
Query 原生全量确定性扫描
          ↓
Neutral Aggregate
          ↓
配置化 Policy Evaluation
          ↓
Candidate Manifest
          ↓
AI 定向 MCP 深查 + 对应版本源码核查
          ↓
可审计的分析报告与证据附件
```

核心目标：

- Query 扫描全部有效数据，不按12帧、固定窗口或模型上下文裁剪。
- Python 不再读取 Trace、扫描 Zone、计算统计或生成候选。
- AI 不处理数百万条原始事件，不修改 Query 计算出的数值。
- 每个重要结论同时给出统计、事件、关系、调用栈、源码、反证、限制和复测方向。
- 自动分析结果可以通过 MCP 事件ID、Profiler GUI和源码逐项复核。
- 长 Trace 使用有界内存、可取消、可恢复的异步扫描。

## 2. 明确不属于本次工作流的内容

本 Skill 仍然只诊断一次捕获，不负责：

- 多运行 A/B 或 matched-runs 比较。
- 3-pair、7-pair工具链性能门禁。
- 自动修改、编译或提交 Unity/Tracy 源码。
- 自动修复性能问题。
- 根据帧形状猜测登录、加载、战斗或稳定玩法阶段。
- 把时间重叠自动判定为因果关系。
- 在 GPU L0 缺少精确 OriginFrameId 时伪造完整 GPU 帧边界。
- 以单 Trace 推断“关闭 Tracy 后 FPS 一定提高多少”。

上述能力后续需要独立设计，不混入单 Trace 诊断 Skill。

## 3. 权威边界

### 3.1 Tracy Query MCP

Query 是唯一允许读取 `.tracy`、N30 Session Canonical/Derived 或 Query Index 的组件。它负责：

- Trace身份、能力、数据域质量和完整性。
- 全帧、全Zone、全Job、全Pass和全内存生命周期扫描。
- Zone树还原和同一时间线上的exclusive/inclusive计算。
- 精确分位数、Top、频率、预算触发、MAD异常、Burst和周期候选。
- 稳定签名、逻辑汇总、候选族和代表帧选择。
- Neutral Aggregate、Policy Evaluation和Candidate Manifest。
- 扫描缓存、进度、取消、恢复和校验。

Query只产生Trace事实、确定性派生事实和配置驱动的候选，不产生业务根因或优化建议。

### 3.2 外置 Analysis Profile

项目预算、筛选规则、源码路径和输出位置由 Skill 外部的单一配置文件控制：

```text
JNTracy.AnalysisProfile.yaml
```

它是运行时唯一配置权威。`SKILL.md` 和说明文档只解释字段，不复制一份可执行预算。

### 3.3 AI

AI负责：

- 调用常驻 Query MCP并监控异步扫描。
- 根据Candidate Manifest对候选进行定向深查。
- 验证调用栈、typed relation、资源生命周期和源码版本。
- 建立、验证和排除假设。
- 区分可以证明、不能证明和需要补充的数据。
- 给出原因解释、优化方向、风险和复测标准。
- 生成可读的主报告与专项报告。

AI不得重新计算或改写Query给出的百分位、排名、exclusive time、频率、容量和候选触发原因。

### 3.4 Python与PowerShell

正式工作流中：

- Python不得扫描Trace或Session。
- Python不得遍历全量Zone、Frame、Job、GPU Pass或Allocation。
- Python不得成为候选生成或统计权威。
- PowerShell只负责启动、停止、路径准备和轻量文件验证。
- 当前Python报告生成器可以在迁移初期继续作为确定性Renderer，但输入只能是Query结果和AI结构化结论，不能重新计算性能数据。
- Python可以保留为Query开发期的小型Oracle、测试和报告渲染工具，不属于生产扫描路径。

## 4. 外置配置设计

### 4.1 单一权威文件

`JNTracy.AnalysisProfile.yaml` 至少包含：

```yaml
schema: 1

project:
  id: jn-unity-t3
  locale: zh-CN

paths:
  query_executable: C:\CodeProjects\GodotProjects\tracy-0.13.1\build-latest\Release\tracy-query.exe
  profiler_executable: C:\CodeProjects\GodotProjects\tracy-0.13.1\build-latest\Release\tracy-profiler.exe
  analysis_output_root: C:\Users\Admin\Documents\JN-Unity-T3\Analysis
  query_cache_root: C:\Users\Admin\Documents\JN-Unity-T3\Analysis\.jn-query-scan

source_repositories:
  unity_engine:
    path: C:\workflow\jnunity
    revision_from: app_info.engine_revision
  package_repo:
    path: C:\workflow\PackageRepo
    revision_from: app_info.package_revision
  tracy:
    path: C:\CodeProjects\GodotProjects\tracy-0.13.1
    revision_from: app_info.tracy_revision

```

同一文件还必须具有 `budgets`、`scan_policy`、`domain_policy`、`marker_attribution`、`path_mappings` 和 `report_policy` 六个顶层对象；其完整字段由配套Schema定义，不允许依赖隐藏默认配置。

具体机器路径和项目预算不写入通用 `SKILL.md`。Skill发布给其他成员时同时发布Schema和模板，每台机器维护自己的Profile。

### 4.2 已确认预算

当前权威预算：

| 指标 | 值 | 权威状态 | 用途 |
|---|---:|---|---|
| `Player.Frame` | 16.666667 ms | Confirmed | 60 FPS帧墙钟预算候选触发 |
| GPU Local Memory | 6.4 GB，十进制 | Confirmed | DXGI Local容量候选触发 |
| Process Memory | 16 GB，十进制 | Provisional | 进程内存候选触发，报告必须标注暂定 |

已有Lua、Render、UGUI、Animation、Physics、ParticleSystem、WaitForJobGroupId等模块预算只能作为 `planning_reference` 或独立告警上限。只有满足以下条件才能触发对应预算候选：

- Marker归属明确。
- 使用正确的exclusive或wait口径。
- 覆盖率达到配置要求。
- 预算scope、thread、frame-set和metric与事件一致。

这些模块预算不能直接相加为帧预算，也不能自动产生根因结论。

尚无权威预算的Render Thread、GPU总帧、单GPU Pass、Job Worker和I/O只做排名、分布和异常分析。

### 4.3 Profile解析

运行开始时：

1. 校验YAML Schema。
2. 解析并标准化全部默认值和路径。
3. 将标准化配置传给 `analysis.scan.start`。
4. Query计算 `config_hash`。
5. 在分析目录保存原始Profile、标准化快照和SHA-256。
6. 临时覆盖只进入本次快照，不回写用户配置。

## 5. Query异步扫描接口

新增通用接口族：

```text
analysis.scan.profile.validate
analysis.scan.start
analysis.scan.status
analysis.scan.cancel
analysis.scan.resume
analysis.scan.summary
analysis.scan.signatures
analysis.scan.candidates
analysis.scan.candidate.get
analysis.scan.representative_frames
analysis.scan.quality
analysis.scan.close
```

### 5.1 `analysis.scan.start`

输入至少包括：

- `trace_id` 或 Session generation。
- 标准化Scan Profile。
- 输出目录。
- 是否允许复用已有Neutral Aggregate。
- 用户明确关注项。

输出：

```text
scan_id
trace_identity
aggregate_identity
policy_identity
state
reused
```

### 5.2 异步状态

扫描状态：

```text
Created
Preflight
CommonScan
DomainScan
CrossDomainScan
NeutralAudit
PolicyEvaluation
CandidateAudit
Completed
CancelledResumable
Failed
```

`analysis.scan.status` 返回阶段、输入进度、各域进度、内存、缓存、已处理事件数、预计剩余和最后错误。不得连续120秒无进度且无解释。

### 5.3 缓存身份

Neutral Aggregate身份：

```text
Trace strong identity
+ Query executable identity
+ Query schema
+ Scan algorithm version
+ Neutral aggregate schema
```

Policy结果身份：

```text
Neutral Aggregate identity
+ normalized AnalysisProfile config hash
+ Policy algorithm version
+ user focus hash
```

修改预算、Top范围或MAD阈值只重建Policy Evaluation，不重新扫描Trace。修改Zone exclusive算法或稳定签名算法才重建Neutral Aggregate。

### 5.4 失败、重试和恢复

- 每个逻辑操作最多尝试3次。
- 文件短暂占用、MCP连接中断等瞬时错误可以重试。
- Trace身份变化、Schema不兼容、checksum失败和确定性数据损坏不重试。
- 用户取消后保存安全检查点，可继续扫描。
- 未完成或校验失败的扫描结果不能进入正式报告。
- 源数据单域失效时，其他有效域仍可分析。
- Query扫描自身造成的丢失、计数不一致或缓存损坏使整个对应generation失效。

## 6. 两层确定性分析

### 6.1 Neutral Aggregate

第一层是与项目预算无关的中立事实聚合：

```text
Trace / Session
→ 全量事件遍历
→ 树、生命周期、关系和per-frame精确数据
→ Neutral Aggregate
```

它包括：

- 完整Frame/FrameSet分布。
- CPU/GPU Zone结构化签名。
- inclusive、exclusive、自身等待和子节点union。
- Job Schedule/Ready/Slice/Complete/Wait关系。
- CPU/GPU内存生命周期与容量。
- Sampling/Context Switch分布。
- GPU Pass、Queue、Range和资源关系。
- I/O生命周期。
- Tracy telemetry成本事实。
- 所有数据域的present/status/reason/quality。

### 6.2 Policy Evaluation

第二层使用外置Profile：

```text
Neutral Aggregate
+ Budget/Top/Anomaly/Capacity/Quality/UserFocus规则
→ Candidate Manifest
```

Policy Evaluation必须确定性运行。相同Aggregate、配置和算法版本产生相同候选、顺序和代表帧。

## 7. 全量扫描和精确稀疏序列

### 7.1 全量原则

- 默认扫描整个Capture的所有完整帧。
- 用户指定范围时生成明确的Analysis Window。
- 没有全局12帧或固定样本上限。
- 所有有效帧都参与数值计算。
- 代表帧仅用于深查，不用于替代全量统计。

### 7.2 稀疏per-frame序列

不建立“签名数量 × 总帧数”的稠密矩阵。每个稳定签名只保存实际出现的帧或时间桶：

```text
Signature
→ Frame/Time
  → occurrence_count
  → inclusive_time
  → exclusive_time
  → wait_time
  → maximum_occurrence
  → representative_event_ids
```

要求：

- p50/p90/p95/p99使用精确值，不使用近似Sketch。
- 大数据量排序使用有界内存的磁盘run和external merge。
- Frame、time和event ID使用64位键，不设置最大帧号。
- Burst与周期检测读取精确稀疏序列。
- 不复制全量原始Zone字段；具体事件仍通过MCP回查。

必须同时提供两种统计：

1. `per_complete_frame`：事件未出现的完整帧计0，用于回答系统平均占用多少帧预算。
2. `when_present`：只统计事件实际出现的帧，用于回答事件执行时有多重。

报告不得混淆这两种分母。

## 8. Zone树与真正热点归属

### 8.1 全深度扫描

Query扫描每个有效Zone层级直到最深叶节点，不固定L0/L1/L2深度。每层的自身执行时间都可以成为候选。

同一线程或同一GPU Queue时间线中：

```text
inclusive = end - begin
exclusive = inclusive - union(direct child intervals)
```

使用子区间union而不是简单相加，避免异常重叠重复扣除。父子时间错误、负区间或不合法嵌套进入Data Quality，不通过静默截断伪装修复。

### 8.2 三种排名

每个适用域生成：

- `Exclusive Work Top`：定位真正执行工作。
- `Inclusive Impact Top`：定位影响范围和上下文。
- `Wait/Critical Path Top`：定位同步、Fence、Present和Job等待。

父子候选通过Candidate Family合并。主报告展示最有解释力的节点和完整祖先路径，而不是把树上每一层重复报告为独立问题。

### 8.3 跨线程和跨Queue

- 不把不同线程的墙钟时间直接相加。
- 不把CPU与GPU异步时间直接相加。
- 不在不同真实GPU Queue之间做exclusive扣减。
- 区分latency contribution、work volume、parallel work和blocking wait。
- Sampling用于解释高self time或未插桩区域，不替代Zone时间。

## 9. 稳定签名和逻辑汇总

### 9.1 结构化精确签名

聚合键至少包含：

```text
Domain
+ FrameSet
+ ThreadRole / GPU Queue
+ normalized full ancestor path
+ Zone/Pass/Job name
+ SourceLocation / Callsite
+ Taxonomy（适用时）
```

动态帧号、时间戳、pointer、对象地址、一次性Job ID和CommandList ID不能进入稳定签名。

域级规则：

- CPU Zone：线程角色、完整祖先路径、SourceLocation。
- GPU Pass：真实Queue、完整taxonomy path、Pass source。
- Job：Job类型、Stage、Schedule Callsite和线程类别。
- C#/Lua：Source Stack中的类、函数、文件和行。
- CPU Allocation：MemLabel、Allocator、大小区间和创建Callsite。
- GPU Memory：资源类型、名称类别、Allocation类型和Pass关系。
- I/O：子系统、阶段和路径类别，不使用绝对路径。
- Sampling：标准化调用栈或叶函数路径。

### 9.2 Logical Rollup

在精确签名之上，Query按稳定名称、Callsite或子系统生成跨上下文逻辑汇总，用于发现同一系统的累计压力。精确签名负责定位，Logical Rollup负责总体影响；两者必须有明确关系，不能互相替换。

## 10. Raw与Robust双统计

每个签名和域同时输出：

### Raw

- 全部完整帧的p50/p90/p95/p99/max。
- 超预算次数和比例。
- 时间分布和累计贡献。
- 每分钟或每千帧出现频率。

### Robust

- median和MAD基线。
- 与稳定基线的delta。
- 异常帧序列和时间聚类。
- IsolatedSpike、RecurrentSpike、BurstWindow、PersistentPressure分类。

异常峰值不会从Raw总体统计中删除。只有结构不完整、时间戳非法或Capture边界不完整的记录可以排除，且必须保存排除原因和数量。

当前默认显著性参数由Profile控制，初始采用：

- 稳定CPU/GPU候选p95每帧至少0.25 ms且影响至少1%完整帧。
- 孤立局部delta至少2 ms。
- 累计贡献至少占该域exclusive或去重时间1%。
- 同结构重复至少3次且分布在至少2个时间区段。
- 每域至少评估Top 10；继续扩展到累计贡献80%；上限50。

这些值是候选筛选参数，不是产品预算或根因判定。

## 11. 多候选池和确定性优先级

候选入口：

```text
BudgetCandidates
TopCandidates
AnomalyCandidates
CapacityCandidates
QualityCandidates
UserFocusCandidates
```

同一结构问题从多个入口命中时合并为一个Candidate Family，保留全部触发理由。

优先级：

| 优先级 | 含义 |
|---|---|
| P0 | 数据失效、Core gap、崩溃风险或容量越界使分析不可信 |
| P1 | 权威预算持续越界，或位于已证明关键路径 |
| P2 | Top贡献明显、频繁出现或形成稳定Burst |
| P3 | 单次严重峰值、相关性线索或证据待补 |
| P4 | 信息性观察和调查积压 |

不生成跨域的单一加权总分。不同单位、预算权威和证据强度分别保留，防止权重掩盖事实。

## 12. GPU分析边界

### 12.1 当前已知限制

当前 `GPU.Frame.Direct` 是每个D3D12 CommandList的物理L0片段，不是完整GPU帧。一帧通常包含多个L0，且正常渲染与UI可能表现为两个连续L0组。

本次Skill重构不要求先实施 `OriginFrameId → L0 Segment` 插桩改造。

### 12.2 双轨GPU候选

Track A：CPU Frame驱动分析：

```text
异常Player.Frame
→ CPU/Render/Job事实
→ 具有明确Frame关系的GPU Pass
→ 同时间范围GPU上下文
```

Track B：GPU Pass独立分析：

```text
全Trace GPU Pass
→ total/p95/max/frequency
→ recurrent spike
→ queue gap
→ taxonomy gap
```

### 12.3 GPU帧证据级别

- 所有子Pass携带一致、明确FrameId时，可以构造 `InferredFromChildPasses` GPU范围。
- 只有时间相邻或视觉连续时，只能标为 `TemporalCandidate`。
- 未归属L0必须统计和报告。
- GPU总帧预算未定义，且关联不完整时，不生成确定性的逐帧GPU预算结论。
- 单个GPU Pass/Zone时间、真实Queue内排序和全Trace贡献仍然可以精确统计。

## 13. CPU、Job、Memory和I/O语义

### 13.1 CPU与主线程

- `Player.Frame`使用墙钟预算。
- Zone热点主要使用exclusive time。
- 父节点使用inclusive说明影响范围。
- Wait独立分类，不能写成CPU执行时间。
- Main→Render→Job关系缺失时只能形成相关性候选。

### 13.2 Job

优先使用真实关系：

```text
Frame
→ Schedule
→ Ready/Queue
→ Worker Slice
→ Complete
→ Wait/Continuation
```

无waiter时不得构造虚假返回线。关系、Slice或Wait数据缺失时必须给出具体不可用原因。

### 13.3 CPU Memory

区分：

```text
CapacityPressure
GrowthTrend
Churn
LeakCandidate
TransientSpike
AccountingGap
```

短期增长不能直接写成泄漏。Allocation调用栈只在Trace确实存在时使用。

### 13.4 GPU Memory

固定区分：

- DXGI current usage/budget。
- EngineKnownPhysical。
- Allocation/Heap/alias。
- Resource capacity/logical bytes。
- owned/physical/resident bytes。
- referenced working set。
- direct/inclusive range bytes。
- untracked bytes。

GPU Catalog失效时仍可分析DXGI和独立物理总量，但Resource名称、类型、Range和Pass来源必须标为不可用，不能通过时序猜测。

### 13.5 I/O

按request/config/queue/start/read/decompress/complete/cancel/error生命周期分析。中途连接造成的历史缺失必须与真正的orphan或drop区分。

## 14. Tracy Telemetry Cost独立域

单次Trace能够直接证明：

- 插桩函数的测得self cost。
- producer事件、字节、调用栈和队列积压。
- Tracy worker/capture线程CPU占用。
- FrameImage处理成本。
- Sampling、Context Switch和Allocation Stack生产者成本。
- stream写盘吞吐、overflow、drop和degrade。
- Tracy线程与Main/Render线程的可观察CPU竞争。

单次Trace不能直接证明：

- 关闭Tracy后FPS提高多少。
- 某个数据域关闭后Player.Frame必然减少多少。
- Present、调度或GPU等待的反事实变化。

Query和报告使用以下语义：

```text
MeasuredSelfCost
ObservedSystemPressure
EstimatedAttribution
RequiresABValidation
```

主报告固定包含：

```text
TotalCaptureOverhead = NotMeasuredSingleTrace
```

Tracy相关问题必须在主报告中单列，并生成 `07-Tracy-Telemetry-Overhead.md`。

## 15. Candidate Manifest

每个候选至少包含：

```text
candidate_id
candidate_family_id
domain
structural_signature
logical_rollup
metric
raw_statistics
robust_statistics
rank
cumulative_contribution
budget_reference
trigger_reasons
priority
affected_frames
total_complete_frames
inclusive_time
exclusive_time
wait_time
ancestor_path
representative_frames
supporting_event_ids
relation_provenance
trace_identity
aggregate_identity
policy_identity
quality_status
```

候选数值和排序是Query权威结果。AI只能追加调查状态、证据引用、假设、源码和结论，不得重写这些字段。

## 16. AI深度调查策略

### 16.1 强制范围

- 所有P0。
- 所有P1。
- 所有用户明确指定项。
- 每域P2 Top 10；不足80%累计贡献时继续扩展，最多50项。
- P3中的所有重复峰值和Burst。
- 每类孤立峰值最严重的3项。
- P4默认进入附录。

### 16.2 代表帧

每个候选按需选择：

- worst 3。
- typical bad 2。
- first和last。
- 每种不同调用树、线程或GPU签名至少1个。
- Burst的start、peak和end。
- 周期问题从不同时间段选取实例。
- 必要时增加正常counterexample。

所有帧参与统计；代表帧只用于证据深查。

### 16.3 调查步骤

```text
候选来源
→ Raw/Robust统计
→ 代表帧
→ 完整父路径和exclusive/inclusive
→ 线程/Queue上下文
→ typed relation/生命周期
→ Callstack/Source Stack
→ Sampling或资源证据
→ 对应revision源码
→ 假设支持与排除
→ 结论、限制、方向和复测
```

### 16.4 停止条件

正式完成前：

1. 所有强制候选获得 `Confirmed`、`StronglySupported`、`Suspected` 或 `Unresolved`。
2. 每个结论有可回查MCP事实ID。
3. 未调查候选全部进入 `InvestigationBacklog`。
4. 报告披露扫描覆盖率、调查覆盖率和未覆盖贡献率。
5. 数据质量不足时停止提升置信度，不使用经验填空。

## 17. 证据与置信度

事实类型：

```text
ExactTraceFact
DeterministicDerivedFact
Correlation
CodeHypothesis
```

结论级别：

| 级别 | 要求 |
|---|---|
| Confirmed | 精确Trace链与源码、Sampling或独立证据闭环 |
| StronglySupported | 确定性统计加至少一个独立支持证据 |
| Suspected | 只有相关性、共现或代码机制推断 |
| Unresolved | 数据缺失、失效、冲突或查询不可用 |

时间重叠和相邻不能单独证明因果。每个问题必须同时写支持证据、反证、限制、可以证明、不能证明和最小补证要求。

## 18. 源码关联

源码链：

```text
Candidate/Event ID
→ SourceLocation/Callsite/StackRef
→ Symbol/Module
→ Profile path mapping
→ Git revision验证
→ 文件和行号
→ AI代码分析
```

规则：

- Trace revision与本地HEAD一致时可使用当前工作树。
- 本地仓库包含目标revision但当前HEAD不同，应读取目标revision内容。
- 找不到目标revision时标记 `source_revision_mismatch`，源码只能作为假设。
- 准确文件、行号、调用栈和执行证据可以支持源码级确认。
- 仅通过同名文本搜索找到的代码最多支持 `Suspected`。
- C#/Lua Source Stack与Native Callstack分别保存，不能互相替代。
- 无栈时仍可报告时间事实，但必须说明无法证明调用来源。
- 代码推断不能覆盖Query数值和typed relation事实。

## 19. 扫描产物

正式扫描不导出全量原始事件JSONL，只保存完整聚合签名和候选结果：

```text
<AnalysisRun>\
├─ run-manifest.json
├─ resolved-analysis-profile.yaml
├─ analysis-state.json
├─ scan\
│  ├─ status.json
│  ├─ domain-summary.json
│  ├─ ranked-signatures.jsonl
│  ├─ candidates.jsonl
│  ├─ candidate-families.json
│  ├─ representative-frames.json
│  ├─ data-quality.json
│  └─ query-scan.log
├─ evidence\
└─ report\
```

`ranked-signatures.jsonl` 保存全部稳定签名聚合，而不是只保存入选候选，确保可以审计漏选和排名。它不保存每一次原始事件副本。

每个文件记录：

- Trace、Aggregate和Policy identity。
- Schema和算法版本。
- 配置哈希。
- 行数和字节数。
- SHA-256。
- 完整性状态。

未完成或校验失败的扫描目录不可作为报告输入。

## 20. 可恢复Skill状态机

```text
Created
→ ValidatingProfile
→ ValidatingTrace
→ StartingMcp
→ StartingScan
→ WaitingForNeutralAggregate
→ EvaluatingPolicy
→ DiagnosingCandidates
→ VerifyingSource
→ AssemblingEvidence
→ RenderingReport
→ ValidatingReport
→ Completed / CompletedWithDegradation
```

状态文件保存：

- Trace、工具、算法和配置身份。
- MCP进程身份和 `scan_id`。
- Query状态、重试和恢复记录。
- 已完成、待处理和失败候选。
- MCP查询游标及证据路径。
- 源码核查状态。
- 报告生成和验证状态。
- 最后安全检查点。

每个候选完成后立即落盘。任务中断后从最后检查点继续，不重复已经完成的调查。

## 21. 报告包

```text
<AnalysisRun>\report\
├─ Performance-Analysis-Report.md
├─ 01-Capture-and-Data-Quality.md
├─ 02-Candidate-Inventory.md
├─ 03-CPU-Frame-and-Thread.md
├─ 04-GPU-Pass-and-Memory.md
├─ 05-Job-Sampling-and-Wait.md
├─ 06-Managed-Lua-Memory-IO.md
├─ 07-Tracy-Telemetry-Overhead.md
├─ 08-Profiler-Manual-Verification-Guide.md
└─ 09-Optimization-Roadmap.md
```

### 21.1 主报告

主报告包括：

1. 一页结论摘要。
2. 捕获身份、能力和结论适用范围。
3. 关键5～10个问题。
4. 全域总体状态。
5. Tracy自身成本。
6. Investigation Backlog和Evidence Gaps。
7. 按收益、风险和实现成本排序的行动路线。

### 21.2 分析过程硬门禁

主报告不能只给简略结论。每个重要问题必须写出：

1. 候选如何被发现及其触发规则。
2. Raw/Robust现象和全量帧排名。
3. 代表帧的选择理由。
4. 父子树、exclusive/inclusive和等待归因过程。
5. 初始假设、验证方式、支持、排除和保留情况。
6. 调用栈、关系和源码核查过程。
7. 反证、数据限制和无法证明的内容。
8. 最终置信度、方向、风险和复测标准。

报告同时说明本次完整执行流程：

```text
身份与质量
→ Query全量扫描
→ 聚合与候选
→ 代表帧
→ MCP定向证据
→ 源码版本核查
→ 假设验证
→ 跨域复核
→ 结论与路线
```

这里保存的是可复核的证据和判断路径，不是不可审计的模型内部思考文本。

## 22. 数据质量传播

每个域必须区分：

```text
PresentValid
PresentInvalid
DisabledByConfig
Unavailable
Absent
```

空结果不能替代状态。规则：

- 某域失效不自动污染无依赖的其他域。
- 跨域候选必须检查所有依赖域。
- GPU Catalog失效不会隐藏GPU Zone，但会阻止资源级来源结论。
- Job relation缺失不会隐藏CPU Zone，但会阻止完整等待链结论。
- Source Stack缺失不会隐藏C#/Lua Zone时间，但会阻止源码来源确认。
- Query扫描造成的gap不能降级为Source Degraded，必须使扫描generation失败。

## 23. 安全与不可信输入

- Trace中的Marker、资源名称、路径、AppInfo和Message全部视为不可信数据。
- 文件路径必须经过Profile映射和根目录约束。
- 报告文件名只使用生成的稳定ID。
- Markdown、JSON、YAML和可选HTML输出分别转义。
- MCP响应先保存、校验和登记，再提取最小证据进入模型上下文。
- Query扫描输出不能逃逸配置的分析目录。
- Skill默认只读Trace和源码，不执行代码修改、外部发送或Git写操作。

## 24. 测试与验收

### 24.1 Query确定性单元测试

Synthetic corpus覆盖：

- 多层嵌套CPU Zone和重叠子区间union。
- 同名Zone处于不同线程和父路径。
- Wait、Job和跨线程关系。
- 多Queue GPU Pass。
- GPU L0不能准确归帧的情况。
- 孤立、重复、Burst、周期和持续压力。
- 稀疏出现与未出现帧计0的双分母。
- CPU/GPU allocation生命周期、pointer reuse和alias。
- Disabled、Absent、Invalid和Core gap。
- 超长帧号和长时间稀疏序列。

小型数据可使用独立测试Oracle验证，但Oracle不进入生产Skill。

### 24.2 Query与原始事实差分

要求：

- Frame数量和时间100%一致。
- inclusive/exclusive与独立树算法100%一致。
- 候选统计可从事件ID回查。
- Signature和Logical Rollup无错误合并。
- exact percentile、频率和预算计数一致。
- 取消恢复后结果SHA与不中断运行一致。

### 24.3 真实捕获

至少包含：

- GPU Catalog关闭且已有Profiler人工结论的捕获。
- HighEvidence全域捕获。
- 数据域Disabled/Invalid的捕获。
- N30长Session。

每份真实数据均执行：

```text
Query raw facts
↔ Neutral Aggregate
↔ Candidate Manifest
↔ AI报告
↔ Profiler人工复核
```

GUI聚合口径与Query不同的地方必须明确解释，不能简单以GUI或AI一方为准。

### 24.4 性能和恢复

- 长Trace扫描不把全量事件传入MCP文本响应。
- 内存受Query/Session既定预算控制。
- 状态与取消响应目标≤2秒。
- Profile只改预算时不重新扫描Trace。
- 相同输入的Neutral Aggregate和Candidate Manifest确定性一致。
- 临时错误最多重试3次。
- 查询失败、partial、truncated和cursor未完成不能被解释为空数据。

### 24.5 Skill和报告验收

- 所有P0/P1和用户指定项完成调查。
- 每个重要结论有完整分析路径。
- AI未改写Query数值。
- Investigation Backlog和未覆盖贡献完整。
- 源码revision匹配状态明确。
- 主报告易读，专项附件可追溯。
- 报告内链接、事件ID、帧号和源码路径可解析。
- Skill linter和端到端Skill evaluator通过。

## 25. 迁移与发布

采用先并行验证、后原位替换：

1. 保持当前已安装Skill 1.1.2不变。
2. 在独立草案目录实现新版。
3. 在Tracy Query中实现原生扫描能力。
4. 使用Synthetic和真实捕获完成验收。
5. 保存当前Skill完整归档快照。
6. 将新版原位安装为同名 `diagnose-jn-unity-tracy`。
7. 用户调用方式不改变。

新版未通过前，旧Skill仍可回退。Query新增接口不得破坏已有domain查询。

## 26. 当前实现的保留与替换

### 保留

- 单一常驻Query MCP原则。
- Trace身份、capability和quality优先。
- 三次查询尝试与Evidence Gap。
- typed relation和源码证据边界。
- 单Trace Tracy总开销不可测的措辞。
- Evidence Manifest、确定性报告和静态附件原则。
- 录制、停止和转换辅助脚本中与分析扫描无关的安全能力。

### 替换

- PowerShell按窗口分页扫描全部Frame。
- Python计算分布、聚类、显著性和候选。
- AI手动拼接全量统计。
- 四份运行时JSON配置的重复权威。
- 不按Top N裁剪且没有全局调查上限的旧候选规则。
- 固定的三实例代表帧逻辑。
- 将所有候选强制达到同一L6深度的不可控流程。

## 27. 完成定义

只有同时满足以下条件，新工作流才可以替换当前Skill：

1. Query直接扫描全部有效Trace/Session数据。
2. 正式扫描路径不依赖Python。
3. Neutral Aggregate与Policy Evaluation分离。
4. Profile变化不要求重复扫描Trace事实。
5. 全深度Zone exclusive/inclusive正确。
6. Raw、Robust、Top、预算、容量和质量候选均可审计。
7. 稳定签名不会因动态ID爆炸或错误合并。
8. P0/P1和用户关注项全部深查。
9. GPU帧边界不完整时使用明确推断级别。
10. Tracy自身成本与反事实总开销严格区分。
11. 源码revision不匹配不会生成伪确认结论。
12. 扫描、取消、恢复和缓存身份正确。
13. 主报告包含完整可验证分析过程。
14. Synthetic、真实Trace、Profiler人工复核和长Session验收通过。
15. 旧Skill已归档且可以回退。

## 28. 已冻结决策

- 使用方案C：所有确定性扫描由Query执行。
- Python不属于正式扫描链路。
- Query使用异步scan job，支持status/cancel/resume。
- 使用外置YAML作为单一运行时配置权威。
- 保存全部聚合签名，不导出全量原始事件。
- 使用结构化精确签名与Logical Rollup双层聚合。
- 使用多候选池和确定性优先级，不使用混合总分。
- 全帧扫描、精确稀疏序列和精确分位数。
- AI深查范围有明确覆盖和停止条件。
- 源码结论必须先校验revision。
- 报告包含完整可审计分析过程。
- 先在草案中验证，再原位替换同名Skill。
