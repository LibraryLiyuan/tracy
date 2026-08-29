# N29 GPU Resource Analysis Sidecar 实施计划

## 1. 基线与范围

- 开发 worktree：`C:\CodeProjects\GodotProjects\tracy-0.13.1-n29-gpu-analysis`
- 分支：`feature/JNTracy-N29-GpuResourceAnalysis`
- 基线：`f9956f3e`
- 不修改主目录 `dev`，不自动合并、提交或推送到 `dev`。
- 保持 Tracy Protocol 90、JN trace section 12、GPU Catalog schema 1 和 Query Index 13。
- Query API 升级为 1.33；新增 GPU Analysis manifest/raw/derived schema 1。
- Unity、PackageRepo、Player 和录制配置不变。

N29 把 GPU 资源分析从 Profiler 进程中的一次性巨型 `GpuAnalysisSnapshot` 改为转换期生成的持久 sidecar：

```text
.tracy-stream
→ tracy-stream-convert
├─ .tracy
└─ <Trace>.jn-gpu-resource-analysis
   ├─ manifest
   ├─ raw
   └─ derived/<schema>/<algorithm-id>
```

## 2. 核心设计

### 2.1 Converter

`tracy-stream-convert` 在 Protocol 90 GPU Catalog 存在时默认产生 `.tracy` 和 sidecar。Converter 复用已完成 replay/Catalog resolve 的 Worker 数据，不重读 `.tracy`，不重跑 resolver。`--no-gpu-analysis` 仅作为诊断开关。

`.tracy` 与 sidecar 使用独立原子发布。`.tracy` 成功而 sidecar 失败时，保留有效 `.tracy`，不发布未完成 generation，返回 `trace_complete_analysis_failed`，并保留 Builder 重试入口。

### 2.2 Sidecar 布局

```text
<Trace>.jn-gpu-resource-analysis\
├─ manifest
├─ raw\
│  ├─ catalog / strings
│  ├─ allocations / lifetimes
│  ├─ passes / resource-sets
│  ├─ ranges / relations / taxonomy
│  └─ sparse-frame-directory
└─ derived\1\n29-v1\
   ├─ exact-summary
   ├─ resource / pass / allocation indexes
   ├─ frame shards
   ├─ inclusive-summary
   └─ checkpoint
```

- shard 是 immutable 的，各自有 checksum，通过临时文件+原子 rename 提交。
- 目标 shard 大小 128–256 MiB。
- 帧号是稀疏 64 位键，不按最大帧号预分配。
- Direct ResourceSet 无损保存，支持自适应编码与字典去重。
- Inclusive 持久化 bytes/count/hash；完整成员按需重建。
- raw schema 和 derived algorithm 分开版本化，默认保留当前和上一个成功 derived generation。
- sidecar 硬上限 128 GiB，临时文件上限 8 GiB，保留至少 32 GiB 磁盘可用空间。

### 2.3 Builder

新增 `tracy-gpu-analysis-build.exe`：

- 单 writer lease，包含 PID、heartbeat 和 stale recovery。
- checkpoint、pause、resume、cancel。
- 支持当前帧或资源优先构建。
- Profiler 退出后可继续，完成后自行退出。
- 系统内存或磁盘压力下暂停。
- 取消只删除未提交 shard，不破坏已发布结果。
- 旧 `.tracy` 无 sidecar 时执行一次顺序回填。

### 2.4 Profiler / Query / MCP

GPU sidecar 自包含 Resource、Allocation、Heap、Range、Residency、Pass/Frame/Taxonomy、ResourceSet、名称、类型、容量、质量、exactness 和 EngineKnownPhysical peak。

跨域联查键：

```text
GPU Zone     = gpuContextIndex + gpuQueryId
GPU Pass     = passId
Resource     = GpuResourceId
Allocation   = AllocationId
Source Stack = callsiteId / stackRef
```

Profiler 的 `GPU Memory & Resources` 仅读 sidecar，GUI 主线程不运行完整 SHA、全表扫描或集合 union。界面提供进度、优先请求、Cancel、Retry 和 Clear Cache。Clear Cache 不删除仍被 mmap 的 generation。

Query API 1.33 使用共享 sidecar reader，提供分页、取消、进度和明确 unavailable reason。跨域索引未完成时，GPU 资源仍可查，仅相关字段返回 `cross_domain_index_building`。

### 2.5 身份与兼容

Converter 保存完整 SHA-256。日常快速校验使用 Volume/File ID、size、mtime 和头/中/尾分段哈希。复制、替换或歧义时后台计算完整 SHA；强校验失败立即停用 sidecar。

身份状态：`identity_quick_verified`、`identity_strong_verified`、`identity_pending`、`identity_mismatch`。

旧 `ProfilerCache/.../snapshot.bin` 不再是权威结果；保留小型 in-memory Builder 作为 synthetic correctness oracle。

## 3. 实施阶段

1. N29.0：文档、基线、现有结果和问题复现。
2. N29.1：manifest/raw/derived schema、checksum、generation、原子发布、快/强身份。
3. N29.2：Converter Raw Emitter、稀疏帧目录、Exact global summary 和独立失败语义。
4. N29.3：Builder、lease/heartbeat、checkpoint/resume/cancel 和压力门禁。
5. N29.4：Resource/Allocation/Pass/Frame/Range/Heap/Residency/Churn/Peak 索引。
6. N29.5：Profiler sidecar backend 与可恢复界面状态。
7. N29.6：Query/MCP 1.33、共享 reader、跨域联查、分页和取消。
8. N29.7：旧 Trace 回填、复制/改名/替换、generation 共存、stale lease、崩溃/截断/回收。
9. N29.8：成套构建、真实 fixed-v2 Trace、1 小时等价 synthetic corpus、一致性/性能验收、报告和 N29 分支提交。

## 4. 验收门禁

### 4.1 正确性

- Resource、Allocation、Pass、Range、Relation 数量一致。
- Direct ResourceSet 成员 100% 一致。
- EngineKnownPhysical peak 的字节和时间一致。
- Resource↔Pass 双向一致。
- Inclusive bytes/count/hash 可重复生成。
- pointer reuse、alias、shared allocation 不串代、不双计。
- invalid Catalog 不得被隐藏或伪装修复。
- `.tracy-stream`、`.tracy`、sidecar、Profiler、Query/MCP 结果一致。

### 4.2 取消与恢复

在 Converter replay、`.tracy` write、Raw export、Exact summary、Resource/Pass index、Inclusive rollup 和 Manifest publish 分别取消。Profiler 不崩溃，已提交 shard 保持可读，未提交 shard 不进 manifest，重启只重做未完成部分，其他 Tracy 域不受影响。

### 4.3 性能

| 指标 | 门禁 |
|---|---:|
| Converter raw+summary 增量 | ≤30% |
| Exact summary 打开 | ≤2 s |
| 已索引单帧/资源查询 | ≤2 s |
| 优先构建 | ≤5 s |
| GUI 主线程同步重型工作 | 0 |
| GPU 分析新增稳态/峰值内存 | ≤4 / ≤8 GiB |
| 验收 Trace Profiler+Builder | ≤16 GiB |
| Exact direct set/count/peak/checksum | 100% |
| committed shard checksum failure | 0 |

1 小时等价 synthetic corpus 验证无帧数上限、数亿关系的线性/近线性增长、跨 shard 取消恢复，以及输入翻倍时核心构建时间不超过 2.5 倍。不执行真实 60 分钟 gameplay capture，报告标记 `NotExecuted_Real60MinuteCapture`。

## 5. 完成条件

- Converter、Builder、Profiler 和 Query 成套编译。
- 真实 fixed-v2 Trace 与 synthetic corpus 通过正确性、取消/恢复和性能门禁。
- 生成 N29 验收报告，包含 HEAD、二进制身份、Trace/sidecar 路径、计数、哈希、时间、内存、失败项和回退点。
- 仅提交 `feature/JNTracy-N29-GpuResourceAnalysis`，不合并 `dev`。

## 6. 执行状态（2026-08-30）

| 阶段 | 状态 | 结果 |
|---|---|---|
| N29.0 文档与基线 | Completed | 基线固定为 `f9956f3e`，所有工作位于隔离 worktree。 |
| N29.1 Schema 与身份 | Completed | manifest/raw/derived schema 1、SHA-256、快速/强身份与长路径支持完成。 |
| N29.2 Converter Raw Emitter | Completed | Converter 默认发布 `.tracy` 与 sidecar；独立失败语义和 `--no-gpu-analysis` 完成。 |
| N29.3 Builder 与存储 | Completed | 外部 Builder、lease/heartbeat、dead PID 恢复、checkpoint、cancel/resume 完成。 |
| N29.4 派生索引 | Completed | Resource/Allocation/Pass/Frame/Range/Relation、direct set、peak 和分页索引完成。 |
| N29.5 Profiler | Completed_Static | Profiler 已切换到 sidecar controller，Release 编译和静态门禁通过；锁屏环境未执行最终 GUI 点击验收。 |
| N29.6 Query/MCP | Completed | Query API 1.33、共享 reader、`gpu.pass.by_frame`、分页和 MCP 实测完成。 |
| N29.7 恢复与兼容 | Completed | 长路径、复制/替换身份、active/stale lease、checksum、内部/外部取消恢复完成。 |
| N29.8 集中验收 | Completed_With_Exceptions | 真实 Trace、1 小时等价 synthetic、性能和 MCP 已验收；真实 60 分钟捕获按计划标记未执行。 |

详细证据见：

```text
doc\JN-Tracy-N29-GPU-Resource-Analysis-Sidecar-Acceptance.zh-CN.md
```
