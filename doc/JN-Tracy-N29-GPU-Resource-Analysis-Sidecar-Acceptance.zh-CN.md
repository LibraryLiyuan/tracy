# N29 GPU Resource Analysis Sidecar 验收报告

## 1. 结论

N29 的核心生产路径已经完成：Converter 在转换期生成可独立校验的 GPU Resource Analysis sidecar，外部 Builder 可恢复地建立派生索引，Profiler 和 Query 共用 sidecar reader。真实 Protocol 90 数据、100,224,000 条资源引用 synthetic corpus、取消恢复、长路径和 MCP 快速查询均已通过。

最终状态：`Passed_With_Declared_Exceptions`。

例外不是数据正确性失败：

- `NotExecuted_Real60MinuteCapture`：按计划只运行一小时等价 synthetic corpus，没有重新录制真实 60 分钟 gameplay。
- `ProfilerGuiInteraction_NotExecuted_LockedDesktop`：Profiler Release 编译、controller/static 门禁通过；最终 GUI 点击、Cancel、Retry 和 Clear Cache 人工交互因 Windows 桌面锁定未执行。
- `BuilderPriority_Partial`：已索引 frame/resource 查询和 Profiler 优先读取满足时延门禁；Builder 在正在构建的大 generation 内尚不做细粒度抢占式重排。

以上例外不影响 `.tracy`、sidecar、Query/MCP 的正确性和可用性，但在合并回 `dev` 前建议补一次解锁桌面的 Profiler 人工验收。

## 2. 源码与回退点

| 项目 | 值 |
|---|---|
| Worktree | `C:\CodeProjects\GodotProjects\tracy-0.13.1-n29-gpu-analysis` |
| 分支 | `feature/JNTracy-N29-GpuResourceAnalysis` |
| 基线 | `f9956f3e` (`Fix quadratic GPU Catalog stream conversion`) |
| Protocol | 90 |
| JN trace section | 12 |
| GPU Catalog schema | 1 |
| Query API | 1.33 |
| Query Index | 13 |
| GPU analysis schemas | manifest/raw/derived 1 |
| 回退方法 | 在 N29 分支 revert 最终 N29 提交；`dev` 未被修改、合并或推送。 |

本轮没有修改 Unity、PackageRepo、Player 或录制 INI。

## 3. 交付能力

### 3.1 Converter

- 默认同时生成 `.tracy` 和 `<Trace>.jn-gpu-resource-analysis`。
- replay 完成后直接复用 Worker 的 Catalog/Pass/Range/Relation 数据，不重读刚生成的 `.tracy`，不重复运行 Catalog resolver。
- `.tracy` 与 sidecar 独立发布；sidecar 失败不破坏有效 `.tracy`。
- 支持 `--no-gpu-analysis` 诊断基线。
- 写 Trace 时增量计算 SHA-256。
- Windows 超过 `MAX_PATH` 的 sidecar shard、manifest、lease、hash 和 identity I/O 使用 extended-length path。

### 3.2 Sidecar 与 Builder

- raw shard、exact summary、derived page 分代保存，带 checksum 和临时文件原子提交。
- 帧索引使用稀疏 64 位键；没有按最大帧号分配数组。
- 支持 Resource、Allocation、Pass、Frame、Range、Relation 和 direct ResourceSet 双向索引。
- 单 writer lease 包含 PID 和 heartbeat；活进程阻止第二 writer，dead PID 立即恢复。
- 支持 checkpoint、cancel、resume；取消不把未提交 page/shard写入 manifest。
- 当前和前一成功 derived generation 可共存。
- committed shard checksum 读取前强制验证。

### 3.3 Profiler

- `GPU Memory & Resources` 后端改为 sidecar controller。
- GUI 主线程不再构建完整 `GpuAnalysisSnapshot`，不做全表 union、全文件 SHA 或完整扫描。
- controller 提供状态、进度、Cancel、Retry、Clear Cache 和后台 Builder 管理。
- 清理采用 generation 生命周期保护，不删除仍在使用的 generation。

### 3.4 Query/MCP

- Query API 1.33 统一读取 sidecar。
- 增加稀疏帧接口 `gpu.pass.by_frame`，然后可用 `gpu.pass.resources` 获得该 Pass 的资源集合。
- Resource 搜索、explain、references、frame/pass 查询均支持分页、限制和明确 unavailable reason。
- `FindResources` 批量读取同一 page，避免每个 Resource 重复打开大 page；真实 Pass→Resource 查询从 11.477 s 降至 1.170 s，100m synthetic 为 0.320 s。

## 4. 真实数据集

### 4.1 输入

```text
C:\Users\Admin\Documents\JN-Unity-T3\PlayerCaptures\Admin-HighEvidence-Manual\
G01-Static-20260828-144042\G01-Static-20260828-144042.tracy-stream
```

- 输入大小：1,581,697,486 bytes。
- Protocol events：189,845,861。
- committed revision：120,609。
- GPU reference pass：2,274,056。
- GPU reference use：30,516,722。
- ignored tail：0。
- capture complete：true。

### 4.2 N29 真实输出

```text
C:\Users\Admin\Documents\JN-Unity-T3\PlayerCaptures\Admin-HighEvidence-Manual\
G01-Static-20260828-144042\G01-Static-20260828-144042.N29-with-analysis-nocache.tracy
```

对应 sidecar 为同名 `.tracy.jn-gpu-resource-analysis` 目录。

Exact global summary：

| 指标 | 数值 |
|---|---:|
| Resource | 1,552,183 |
| Allocation | 504,407 |
| Pass | 2,274,056 |
| Reference use | 30,516,722 |
| Logical resource | 2,607,371 |
| Range record | 3,952,718 |
| Relation | 11,295,069 |
| View | 530,306 |
| Part | 5,461 |
| VG record | 383 |
| EngineKnownPhysical peak | 4,059,627,520 bytes |
| Peak time | 66,414,933,136 ns |

质量状态为 usable partial：`core_unresolved=0`，`total_unresolved=7,598`。因此物理/资源核心可查询，但 `exact=false`，不能把 enrichment unresolved 隐藏成完整精确状态。

原始 `fixed-v2.tracy` 只有 Catalog/memory/range，GPU reference pass/use 为 0/0；它用于验证旧 Trace 回填和显存基座，不用于证明 Pass↔Resource。完整关系验证使用本次从 `.tracy-stream` 新转换的 N29 Trace。

## 5. Converter 性能

为了避免 Windows 文件缓存造成不公平比较，最终结论使用同一 1.58 GB Stream 的两次 `scan_cache_used=false` 转换：

| 阶段 | `--no-gpu-analysis` | 默认 sidecar |
|---|---:|---:|
| Scan | 7.405 s | 7.365 s |
| Replay | 15.688 s | 15.666 s |
| Trace write | 4.952 s | 4.764 s |
| GPU analysis raw+summary | 0 | 5.813 s |
| Validate | 2.912 s | 2.944 s |
| Total | 31.065 s | 36.969 s |

- 增量 wall time：5.904 s。
- 相对基线：`+19.0%`。
- 门禁：`≤30%`。
- 结论：Passed。

证据：

```text
C:\Users\Admin\Documents\JN-Unity-T3\N29-Acceptance\
real-convert-no-analysis-nocache-report.json
real-convert-with-analysis-nocache-report.json
```

## 6. MCP/Query 验收

### 6.1 真实 Trace

| 操作 | 时间 |
|---|---:|
| trace.open | 6.995 ms |
| trace.ready | 2.402 ms |
| gpu.catalog.status | 8.750 ms |
| gpu.resource.search | 1,844.842 ms |
| gpu.resource.explain | 1,119.953 ms |
| gpu.pass.by_frame | 297.933 ms |
| gpu.pass.resources | 1,170.389 ms |

- 示例链：Frame 1 → Pass 14 → Resource 3。
- Query private bytes：12,443,648。
- Query working set：17,399,808。
- 所有已索引核心操作均 ≤2 s，Passed。

证据：

```text
C:\Users\Admin\Documents\JN-Unity-T3\N29-Acceptance\mcp-real-long-path-fast-path.json
C:\Users\Admin\Documents\JN-Unity-T3\N29-Acceptance\real-resource-references.json
```

### 6.2 100,224,000 引用 synthetic

| 操作 | 时间 |
|---|---:|
| resource search | 179.114 ms |
| resource explain | 938.620 ms |
| sparse Frame→Pass | 159.187 ms |
| Pass→Resource | 319.656 ms |

- 稀疏 frame id：2,199,023,255,552，证明没有 32 位帧号或密集数组限制。
- Query private/working set：8.49/16.13 MiB。
- 证据：`C:\Users\Admin\Documents\JN-Unity-T3\N29-Acceptance\mcp-sparse-frame-fast-path.json`。

## 7. 一小时等价规模与内存

两组 corpus 使用相同 216,000 帧、432,000 Pass，只将关系量翻倍：

| 项目 | 50,112,000 relations | 100,224,000 relations |
|---|---:|---:|
| 构建时间 | 72.275 s | 146.410 s |
| sidecar bytes | 1,422,813,626 | 2,725,677,843 |
| 峰值 private | 5,152,444,416 | 7,705,509,888 |
| 峰值 working set | 4,041,097,216 | 6,199,435,264 |

- 输入翻倍时间比：`2.026×`，门禁 `≤2.5×`，Passed。
- 峰值 private 约 7.18 GiB，门禁 `≤8 GiB`，Passed。
- 帧号和关系规模无预设上限。
- 状态：`NotExecuted_Real60MinuteCapture`，真实游戏 60 分钟录制不属于本轮执行项。

## 8. 取消、恢复、身份和损坏测试

| 场景 | 结果 |
|---|---|
| 外部 cancel-file | Builder 返回 130，progress=`cancelled`。 |
| 移除 cancel-file 后继续 | 同一 Trace 恢复到 `ready`。 |
| Raw Catalog 取消 | 不发布 `rawComplete`。 |
| Exact summary 取消 | summary 后不发布完整 manifest。 |
| Manifest publish 前取消 | 未完成 generation 不进入权威 manifest。 |
| Derived page 中途取消 | 已提交 page 保留 checksum；继续复用同 generation。 |
| 当前 PID writer lease | 第二 writer 返回 `writer_lease_active`。 |
| dead PID writer lease | 立即回收并成功继续。 |
| committed shard 损坏 | checksum mismatch 被阻断。 |
| Trace 复制/改名 | quick identity 歧义时经 SHA-256 strong verify 后接受。 |
| Trace 内容被替换 | `identity_mismatch`，停止使用 sidecar。 |
| Windows 长路径 | raw、publish、derived、reader 全链通过。 |

外部测试证据：

```text
C:\Users\Admin\Documents\JN-Unity-T3\N29-Acceptance\builder-cancel-progress.json
C:\Users\Admin\Documents\JN-Unity-T3\N29-Acceptance\builder-resume-progress.json
```

## 9. 自动测试与构建

Query/Sidecar CTest：6/6 Passed。

1. `tracy-query-contract`
2. `tracy-gpu-analysis`
3. `tracy-query-mcp-transcript`
4. `tracy-query-version`
5. `tracy-query-doctor`
6. `tracy-gpu-analysis-n29-static`

JSON schema 全部可解析，`git diff --check` 为 0。Profiler 编译只有既有 generated-data dependency、代码页和 `NOMINMAX` warning，无编译错误。

最终 Release 二进制：

| 二进制 | bytes | SHA-256 |
|---|---:|---|
| `build-n29-capture\Release\tracy-stream-convert.exe` | 11,300,352 | `7930C68F01B5CDFAB2C7990A4A9EC08FC9108B34076BF8A7A48AD6C4E5407214` |
| `build-n29-query\Release\tracy-query.exe` | 15,267,840 | `D40A151ADA7FFA8CCCCBB286E5F94875BACF1BF9DED43FC7BA7E1143C99A37BC` |
| `build-n29-query\Release\tracy-gpu-analysis-build.exe` | 11,512,832 | `1F38C058F7FB9BEC956960AD0B764B6980016279883386FCB356D1442CB1F76B` |
| `build-n29-query\Release\tracy-gpu-analysis-corpus.exe` | 518,656 | `A5A93A3D7B5AD9DA30E29052F2298D1600B7A2B76937E257DB46D72947FE9E1B` |
| `build-n29-profiler\Release\tracy-profiler.exe` | 27,313,152 | `5A6CA7B43B6A041B88C7183D2D37ADB54F2FC05BCF5320728074E13C2A35E149` |

## 10. 门禁汇总

| 门禁 | 结果 | 状态 |
|---|---:|---|
| Converter raw+summary 增量 ≤30% | +19.0% | Passed |
| Exact summary 打开 ≤2 s | 8.75 ms status | Passed |
| 已索引单帧/资源查询 ≤2 s | 最慢 1.845 s | Passed |
| GUI 主线程同步重型工作 = 0 | sidecar controller/static gate | Passed_Static |
| GPU 分析峰值内存 ≤8 GiB | 7.18 GiB private | Passed |
| Profiler+Builder ≤16 GiB | Builder 7.18 GiB；Profiler sidecar reader按页 | Passed_Architecture |
| Direct set/count/peak/checksum 100% | synthetic oracle 与真实 summary一致 | Passed |
| committed shard checksum failure = 0 | 正常数据为0；主动损坏可检测 | Passed |
| 输入翻倍构建时间 ≤2.5× | 2.026× | Passed |
| 真实 60 分钟 gameplay | 未执行 | NotExecuted_Real60MinuteCapture |
| Profiler 最终 GUI 点击 | 锁屏未执行 | NotExecuted_LockedDesktop |

## 11. 已知限制与后续建议

1. 真实 Trace 的 Query resource search 为 1.845 s，虽然通过 2 s 门禁但余量较小；资源继续增长时建议增加名称/类型二级索引。
2. Builder 的 frame/resource 优先请求目前主要作用在 sidecar 已可读后的分页查询；若要在数小时 Trace 的首次 derived build 中真正抢占，应增加按 shard 优先队列和安全点。
3. raw-load 阶段取消会保留 raw shard，但重新启动时会重新载入 raw 并恢复 derived checkpoint；不会破坏数据，但不等于进程级内存快照恢复。
4. 在 Windows 桌面解锁后补一次 Profiler 人工流程：打开真实 N29 Trace → GPU Memory & Resources → Cancel → Retry → Clear Cache → 退出后 Builder 继续。
5. 完成本次人工 GUI 验收后，再评估是否将 N29 分支合并回 `dev`；本次实施不会自动合并或推送。
