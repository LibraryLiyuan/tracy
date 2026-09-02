# N30A 全域可信 Session 与 GPU Derived 收口计划

## 1. 目标与边界

N30A暂停局部`.tracy`导出，优先完成：

```text
.tracy-stream
→ 全域Canonical Session
→ Mandatory Derived
→ Final Audit
→ Query/MCP
```

GPU是第一条真实规模纵向闭环，但N30A最终覆盖全部源数据域。Canonical是唯一转换后事实来源；Derived必须可以确定性重建。源数据缺失必须明确标记，Converter自身造成的缺失必须阻止发布。

继续使用：

```text
Branch:   feature/JNTracy-N30-LongTraceSessionStore
Worktree: C:\CodeProjects\GodotProjects\tracy-0.13.1-n30-long-trace-session
Baseline: b121e58c
```

N30A不修改Protocol 90、JN section 12、Unity、PackageRepo、Player或录制配置；不自动合并或推送其他分支。

截至2026-09-02的最终执行状态：

| 阶段 | 状态 |
|---|---|
| A0～A5 | 已完成并提交；G05真实规模GPU正确性通过 |
| A6 | 已完成并提交；短Trace全域回归、Release/G05查询与跨域Evidence验收通过 |
| A7 | 已完成实现与真实30分钟最终验收；等待本阶段文档提交，不合并、不推送 |

A7固定输入已发布为可查询Session：

```text
Source SHA-256: 2AC46C53257CE9027EC67E098FC15070FB911243F6CD311A166EB97547848068
Session generation: n30-1788306859373994-47656
Canonical shards: 402
Indexed records: 2,285,222,848
GPU independent verifier mismatch_count: 0
Release Query acceptance: 24/24
```

Session状态为`CompleteSourceDegraded`：Converter自身造成的gap/unresolved/checksum failure为0；CPU Zone时钟逆序、process-scoped Scheduling gap以及GPU Catalog producer drop/identity gap均来自源录制，已按域显式保留，不被伪装修复。完整证据见验收文档A7章节。

后续独立里程碑：

```text
C：Session语义数据→局部.tracy
P：Profiler直接打开Session及GPU窗口接入
```

## 2. 固定数据模型

### 2.1 GPU Catalog

生产路径删除完整`JnTraceData/GpuAnalysisSnapshot` Catalog依赖。Resource、Allocation、Heap、View、Logical、Relation和Lifetime改为immutable分页表、external sort/merge和稀疏索引。内存只保留当前页、热字典、小型摘要和有界resolver状态。

Full Snapshot只保留为短Trace正确性Oracle，不得进入Session生产路径。

### 2.2 Direct与Inclusive

每个实际GPU Pass永久保存：

```text
Direct Resource/View/Range成员
Direct resource_count
Direct working_set_bytes
Direct set_hash
```

以下稳定节点预计算精确Inclusive Summary：

- Frame/Queue根节点。
- 稳定GPU taxonomy L0/L1/L2。
- 稳定RenderGraph Phase/Pass。
- VG、VSM、TSR、ScreenProbe、Shadow等明确分析节点。

动态高基数节点的Inclusive Exact Members按需从Direct集合确定性重建，分页返回并进入有界LRU缓存。它不是采样或近似；`count/bytes/hash`必须与预计算Summary一致。

### 2.3 发布前Mandatory Derived

必须完成：

- Resource、Allocation、Heap和Lifetime。
- Pass→Resource及Resource→Pass。
- Range/Subresource。
- Physical peak、Residency、类型和名称。
- Direct Summary和稳定节点Inclusive Summary。
- count、bytes、hash、source ordinal和checksum审计。

任一Mandatory Derived失败均不得发布`CURRENT`。

### 2.4 状态语义

所有源数据域统一返回：

```text
complete
present=false
source_degraded
invalid_source_gap
permission_unavailable
invalid_converter_output
```

`CompleteSourceDegraded`只用于忠实保存源端已有缺失。Converter产生的gap、checksum错误、count mismatch或未解析Core关系必须使generation失效。

## 3. Checkpoint、取消与预算

GPU Derived划分为：Catalog Normalize、Direct Set、Reverse Index、Range、Inclusive Summary和Final Audit。每阶段内部按128～256 MiB Shard原子提交。

Checkpoint保存：

- source Canonical Shard和record ordinal。
- 已提交sorted run。
- 当前阶段和Shard。
- committed count/bytes/checksum。
- previous checkpoint hash。

取消行为：

```text
Cancel
→ ≤2秒停止当前低层批次
→ 丢弃未提交Shard
→ 保留所有已校验Shard
→ CancelledResumable
```

恢复最多重做一个未提交Shard；不得在构建开始时无条件删除完整GPU spool。

内存门禁：

```text
GPU Derived稳态目标 ≤6 GiB
Converter总体目标   ≤8 GiB
软上限             12 GiB
硬上限             16 GiB
```

同时核算内部容器、Process Private Bytes、Working Set、mmap驻留页、临时run和分配器开销。达到软上限时停止预取、提前封存Shard并降为单流水线；接近硬上限时安全Checkpoint后返回`resource_limit`。磁盘继续执行256 GiB Session上限与`max(64 GiB, 10%)`安全预留。

## 4. 实施阶段

### A0——恢复可信基线

- 保存当前Protocol replay导出WIP的完整patch、SHA-256、失败测试和根因。
- 将三个实验文件恢复到`b121e58c`，保留既有未跟踪构建目录。
- 修正N30.5/N30.6状态并登记branch、HEAD、工具和语料身份。
- 运行既有短回归并提交`N30.A0`。

门禁：工作树无未知tracked dirty；既有短测试全绿；不执行重型转换。

### A1——GPU Catalog外存化

- 先写会因完整Catalog物化而失败的内存/规模测试。
- 实现Resource、Allocation、Logical、Relation和Lifetime分页表及external merge。
- 处理pointer reuse、alias、shared allocation和开放生命周期。

门禁：短Trace与Worker逐条一致；生产路径不构建完整Catalog Snapshot；内存不随Catalog记录总量线性增长。

### A2——Exact GPU Derived

- 先写Direct成员、反向关系和Inclusive去重失败测试。
- 构建Direct ResourceSet、Range/Subresource、Resource↔Pass和Frame→Pass。
- 生成Direct Summary与稳定节点Inclusive Summary。
- 实现按需Inclusive Exact Members、分页和hash复算。

门禁：成员、count、bytes、hash和双向索引100%一致；shared allocation无双计。

### A3——预算、取消与恢复

- 先写阶段取消、未提交Shard和恢复失败测试。
- 实现Shard事务、Checkpoint、writer lease、heartbeat和stale writer恢复。
- 在解析、排序、merge、union、页面写入、checksum和审计低层循环加入取消点。
- 实现8/12/16 GiB门禁和磁盘压力暂停。

门禁：取消≤2秒；恢复不重复、不丢失；最多重做一个Shard。

### A4——GPU Query/MCP与独立Verifier

- 完成资源、分配、Pass、Range、生命周期、峰值、类型、名称和反向关系查询。
- Query结果明确Direct、Inclusive Summary或Inclusive Members。
- 建立不复用Builder集合代码的独立流式Verifier。
- Verifier独立计算source count、relation count、bytes、hash和physical peak。

门禁：Query/MCP/Manifest固定同一generation；分页、取消和16 MiB响应限制通过；Verifier与Derived 100%一致。

### A5——GPU真实规模门禁

- 先执行G05固定压力语料。
- G05用于GPU生产路径、长路径、恢复和独立Verifier的真实规模门禁。
- 为避免重复消耗19 GiB语料转换时间，真实30分钟统一留到A7做一次全域最终验收。
- 记录各阶段耗时、Private Bytes、Working Set、mmap、磁盘、吞吐、取消和Checkpoint。

门禁：Converter≤16 GiB、Cancel≤2秒、Direct/Reverse/Range/Peak/hash 100%一致、Core gap/unresolved=0、GPU generation成功发布。

### A6——其他数据域收口

覆盖Frame、Thread、CPU/GPU Zone、Job/Gfx、Sampling/Context Switch、Hardware Sample、CPU Memory、C#/Lua Source Stack、I/O、Lock、Plot、Message、FrameImage、Source/Symbol/Callstack、Runtime Domain、AppInfo和质量事件。

- 所有源存在域满足Inventory count=Canonical committed count。
- 高基数域使用分页/范围Reader，不返回全量vector。
- Runtime Script语义结果改为分页或磁盘索引。
- 全局Job critical path使用Derived DAG或等价有界磁盘算法。
- Gfx统计如真实延迟超标，增加确定性摘要索引。
- 完成Frame→CPU/Script→Job→GPU Pass→Resource/Memory跨域链。

门禁：短Trace全域差分通过；源缺失状态准确；无未报告Converter gap。

### A7——Session全域最终审计

- 执行第二次、也是最后一次真实30分钟转换。
- 对全部源存在域执行Inventory、Canonical、Derived、Query/MCP一致性审计。
- 生成完整验收报告、工具/输入SHA-256、性能、内存、磁盘和可复现MCP查询。
- 验证原始stream未修改、未删除。
- 通过后提交`N30.A7`；不自动合并或推送。

完成条件：所有源存在域精确完整或明确`source_degraded`，Converter未报告缺失为0，Mandatory Derived有效，Query/MCP全域可用，真实30分钟Session成功发布`CURRENT`。

## 5. 测试策略

所有生产代码遵循RED→GREEN→REFACTOR；每个新行为必须先观察到针对性失败。

日常只运行：

- 单元测试。
- Synthetic Session。
- 固定短Trace。
- Traditional Full Worker与Session差分。
- 相关模块回归。

双层Oracle：

- 短Trace：Full Worker与Session逐条语义、成员和关系100%一致。
- G05/30分钟：独立流式Verifier校验source ordinal、count、bytes、hash、peak和双向关系。

重型运行限制：

- G05只在A5使用。
- 真实30分钟只执行一次：A7全域最终审计。
- 连续120秒无进度且状态不变化时停止。
- 达到软上限后两个Checkpoint周期内内存不下降时停止。
- checksum、source ordinal、磁盘安全线或Core gap失败时立即停止。

## 6. 明确延期

- 局部`.tracy`导出进入方案C；必须由Session Semantic Reader构建Window Export Model，不再裁切delta Protocol字节。
- Profiler Session后端和GPU窗口接入进入方案P。
- Legacy大型`.tracy` GPU窗口保留已知规模限制。
- 不实现录制期分片、正在写入stream查询、Live Session、远程服务或自动删除原stream。
