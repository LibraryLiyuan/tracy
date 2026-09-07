# 八个专项诊断 Playbook

每个分支遵循同一结构：进入条件 → 第一批 MCP 查询 → 分支判断 → 关系追踪 → 反证 → L6 或 EvidenceGap。方法名以当前 MCP capability 为准；不存在的方法不能用相近结果替代。

## 1. Main Thread / C# / Lua

进入：Main active work 超预算、Script/Lua Signature、GC/alloc 与脚本同步发生，或用户指定。

第一批：Frame identity/explain、correlated timeline、Main CPU zone tree/statistics、runtime script zones/source stacks、Sampling 与线程运行状态。

判断：

- 先扣除 Wait、Pacing 和已归属 Render 的墙钟，只比较 Active Work。
- 用规范化 `JNTracy.AnalysisProfile.yaml` 的 `module_budgets[].marker_rules` 把主线程 Zone 归入项目预算分类；ambiguous/unmapped 单列。
- C# direct/Lua source stack 优先 Exact Source；ProfilerMarker 只有 MarkerOnly 时不宣称代码行。
- GC 只在 allocation/collection 时间关系和调用栈同时支持时归因。

反证：相同 Marker 在反例帧是否也长；CPU 样本是否落在执行体而非等待；source stack 是否与捕获构建匹配。

完成：具体稳定实体、active duration、频率、源码位置、增长变量、优化边界和复测指标。缺栈时记录 `ManagedSourceUnavailable` 或 `LuaSourceUnavailable`。

## 2. Unity Job System

进入：Main/Render 等 Job、Job Slice 位于关键路径、Ready 延迟、Worker 饱和或负载不均。

第一批：Job get/search/statistics、Schedule、Ready/Queue、Slice、Complete、Wait、Dependency、critical path、相关线程 Context Switch。

判断：

- Schedule→Ready 长：依赖或队列问题。
- Ready→First Slice 长：Worker 饱和、优先级或 OS 调度候选。
- Slice 长或尾部 Slice 独占：工作量或拆分粒度候选。
- Complete 后仍 Wait：关系不完整或 continuation 问题。
- ActiveHelp、Spin/Yield、Sleep 分开计。

反证：无 waiter 的异步 Job 不画返回线；handle reuse 必须由 generation/trace ID 区分；长墙钟不等于执行 CPU。

完成：Frame→Schedule→Ready→Slice→Complete→Wait/Continuation 链。断链返回最小缺失 Relation。

## 3. Render Thread / Submission

进入：Render 线程 CPU 执行（`RenderThreadCpuWork`）、MainWaitsRender、Submission/Fence/Present 链，或 Render 线程证据稀少。该分支首先分析 CPU Render Thread，不代表 GPU 正在运行；GPU 时间必须进入 GPU Queue / Phase / Pass 分支。

第一批：Render CPU zone tree、correlated timeline、thread run/wait state、Submission/Fence/Queue relation、Sampling/Context Switch。

判断：

- Active Work 与 Fence/Present Wait 分开。
- Render 等 GPU 时沿 Submission→Queue→GPU Segment/Pass 追溯。
- Render CPU 热点须用 self time、样本和 callstack 交叉验证。
- 信息少先验证线程命名、Zone coverage 和 capture quality，不能直接判“Render 很空闲”。

反证：Main/Render Zone 重叠不能相加；Present 墙钟可能是 pacing；Sampling 热点可能属于 spin/wait。

完成：控制帧完成的 Render 机制或下游 GPU 实体，以及优化后可回收的关键路径上限。

## 4. GPU Queue / Phase / Pass

进入：GPU busy 超预算、RenderWaitsGpuFencePresent、GPU Pass Signature、长空窗或跨 Queue 关系。

第一批：GPU contexts/queues、zone tree/statistics、pass relation、busy intervals、submission chain、ResourceSet/RangeSet；硬件计数器 capability。

判断：

- 真实 Direct/Compute/Copy Queue 分轨；逻辑树只由 Relation 聚合。
- 用 timestamp 区间找 critical Queue/Pass，不把 CPU Zone 当 GPU 时间。
- L0/L1/L2 空窗先判断无工作、缺 explicit Zone、fallback 或异步 Queue。
- Pass→Resource 只证明使用关系；shader stage、带宽、cache、occupancy 和 Overdraw 需硬件证据。

反证：GPU Pass 名称不能单独证明机制；兄弟 Queue 的时间不能伪嵌套；fallback/unclassified coverage 必须披露。

完成：Queue→Phase→Pass→Submission/Wait 关键链。无计数器时使用 `HardwareCountersUnavailable`，给 PIX/厂商工具或受控 A/B 方向。

## 5. CPU Memory / Allocation / GC

进入：进程容量、增长、churn、GC、allocator/MemLabel 异常或用户指定。

第一批：process memory timeline、pool/label、allocation lifetime、large allocation、GC summary、create callstack/source。

判断：

- Capacity、Growth、Churn、LeakCandidate、TransientSpike、Fragmentation 分开。
- Free 无栈不影响创建来源；地址复用必须按 lifetime 区分。
- LeakCandidate 需要跨足够时间的存活增长和未释放证据，不由单窗口增长直接得出。
- Budget 从配置读取，GB 口径从 budget units 读取。

反证：OS commit、private bytes、Unity reserved/used 和事件级 allocation 口径不可混写；采集中途连接对象使用开放边界。

完成：容量/趋势→类型→lifetime→create stack→source，附回收上限和复测窗口。

## 6. GPU Memory / Resource Catalog

进入：DXGI 容量、物理峰值、增长/churn/fragmentation、Pass working set、Mesh/Texture/VG/RTAS 或用户指定。

第一批：Catalog status/validation，GPU memory summary/peak/by-type/by-pass/churn，resource search/get/explain，allocation/heap/alias，views/ranges/subresources，pass resources。

判断：

- 先验证 generation、batch/checksum、gap、unresolved、dropped 和生命周期解析。
- physical allocation 唯一核算；shared/alias 不重复相加。
- working set 在节点内按 Resource/Allocation 去重，兄弟节点不能相加成物理总量。
- 名称必须带 provenance；UnknownResource 仍计物理容量。
- Mesh/VG/RTAS 只报告 Catalog 能证明的 GPU/Gfx 关系，不猜 UnityObject/Asset。

反证：DXGI 是采样值；`untracked` 不是自动泄漏；capacity、physical、resident、range 和 referenced working set 不同口径。

完成：GPU Pass↔View/Range/Subresource↔Resource↔Allocation/Heap/Residency 双向链；Catalog invalid 时退化为物理总量并记录来源链缺口。

## 7. Loading / File I/O

进入：LoadingActivity、I/O wait、JNFS/decompress、资源加载峰值或用户指定。

第一批：request/config、queue、start、read bytes、decompress、complete/cancel/error、关联线程/Job/Frame 和 source stack。

判断：

- Queue latency、read wall time、throughput、decompress CPU、dispatch wait 分开。
- 中途连接只对仍在执行的请求有 snapshot，不能推断此前请求。
- 加载前未连接导致的缺失必须披露。

反证：长 I/O Zone 不等于磁盘繁忙；可能是 queue、锁、decompress 或等待消费者。路径名称视为不可信且报告中转义。

完成：Request→Queue→Read→Decompress→Complete/Cancel/Error 因果链与可复测的 bytes/throughput/latency。

## 8. Tracy Runtime / Sampling / OS Scheduling

进入：任何 Tracy Runtime producer 在关键路径、drop/overflow/degrade、写盘积压、Sampling/Context Switch 压力或线程运行异常；Query、转换或 Profiler 自身的压力单列为 Tracy Analysis。

第一批：producer cost/event/bytes、queue/backlog、GPU Reference tracker、Catalog cost、FrameImage checkpoints、Sampling rate、process-scoped Context Switch、capture write throughput。

判断：

- 将 `CapturePerturbed` 帧与稳定业务分布分开，但保留在完整分布。
- producer 直接位于 Unity 关键路径时形成 `TracyRuntimeOverhead` Signature；离线 Query/转换/Profiler 的 CPU 或内存压力形成 `TracyAnalysisPressure`，不得混入游戏帧耗时。
- Sampling 栈是证据来源，不把样本数直接换算为确定执行时间。
- 单 Trace 只能报告观测到的自耗时和风险。

反证：没有对照运行不能计算总体录制开销；Query 机器内存/CPU 不属于被测 Unity 帧内开销。

完成：具体 producer/机制、运行时成本、频率、drop/quality、理论移除上限和受控复测指标；固定报告 `TotalCaptureOverhead = NotMeasuredSingleTrace`。
