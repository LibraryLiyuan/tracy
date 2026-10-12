# 长录：先分诊，再窗口扫描（2.4.0）

## 1. 适用范围

满足任一条件即走本路线：

- 输入是 Capture GUI 事件库模式发布的 `.tracy-stream`（录制 ≥ `store_minutes`，同目录有 `<名称>.tracy-stream.store/manifest.json`）。
- 输入是普通 `.tracy`，但完整帧数超过 20,000，或录制超过 5 分钟。

短录和用户已指定帧号/时间范围的分析仍按原流程走：分析整个 Trace 或用户窗口。

整段全量扫描的耗时随录制长度和事件密度增长：`Player_20261003_223424` 密度下，30 分钟约需 1 小时以上。本路线先由 Query 用一次快速分诊给全部帧标出瓶颈类别，再只对选中的窗口做确定性扫描。所有数值仍来自 Query；AI 不自行统计。

## 2. 打开

`tracy_trace_open` 可以直接打开 `.tracy-stream`：Query 选择当前 schema 的事件库，不需要 `.tracy`。

- 首次打开会导出骨架快照，并校验全部 Zone；30 分钟录制需要数分钟，结果缓存在 journal 旁，之后的打开会复用。
- 轮询 `tracy_trace_status`，直到 ready。不要按固定时长判断失败。
- 事件库缺失或 schema 不符时，Query 回退到 journal 的完整加载，这对长录不可行。此时停止分析，按 [capture-workflow.md](capture-workflow.md) 重建事件库（`tracy-stream-export.exe <journal> --store-only`）。

## 3. 分诊

先完成 Profile 校验，再调用：

```text
tracy_scan(operation="triage", trace_id, profile[, frame_set, segment_seconds, window_seconds, max_windows])
```

- `frame_set`：默认 `Player.Frame`，否则取第一个 FrameSet。
- `window_seconds`：推荐窗口的长度，默认 5 s。
- `max_windows`：推荐窗口数上限，默认 6，最多 32。

完整响应原子落盘为 `evidence/triage.json`，并登记 MCP Ledger。30 分钟录制的分诊约 20 s，不需要分析缓存。

响应内容：

| 字段 | 含义 |
|---|---|
| `frame_time` | 全部完整帧的 p50/p90/p99/max、超预算帧数与 Budget Debt |
| `limiters` | 各瓶颈类别的帧数、Debt 及其占比（按 Debt 排序） |
| `segments` | 时间线分段（默认最多 60 段）：帧时间分位、Debt、主导瓶颈，以及主线程活跃、主线程等待、渲染线程活跃、GPU 忙、Job Worker 忙的时间占比 |
| `spike_clusters` | 尖峰簇（至少 2 倍预算，且至少为本段中位数的 1.5 倍）：Debt、主导瓶颈、最差帧 ref |
| `recommended_windows` / `scan_windows` | 推荐窗口及推荐原因；`scan_windows` 可原样传给 start |
| `signals` / `missing_evidence` | 实际读到的线程、GPU Context、显存与内存曲线，以及缺失的信号 |

瓶颈类别沿用 Primary Limiter 枚举：

| `bound` | 类别 |
|---|---|
| cpu_main | `MainThreadCpuWork` |
| cpu_jobs | `MainWaitsJob`、`JobWorkerCpuWork` |
| cpu_render | `RenderThreadCpuWork`、`MainWaitsRender` |
| gpu | `GpuCriticalPath`、`RenderWaitsGpuFencePresent` |
| vram | `GpuMemoryPressure` |
| cpu_memory | `CpuMemoryPressure` |
| io_loading | `LoadingIo` |
| pacing | `IntentionalPacing` |
| 其他 | `Mixed`、`Unknown`、`WithinBudget` |

分诊标签由每帧的时间占比得出：Zone 名称区分等待与工作，另外参考 GPU 忙时间、显存与内存计数器以及 I/O。标签只是假设，不是根因：

- 报告只能把它写成"分诊推断"，并引用 `triage.json` 中的对应段、尖峰簇或窗口。
- 只有窗口扫描的候选、`frame.explain` 或 `frame.critical_path` 证据成立后，才能写 Primary Limiter 结论。
- `missing_evidence` 中的信号缺失时，相关类别不能写成否定性结论。例如没有 GPU Zone，就不能说"非 GPU 瓶颈"。
- 出现 `vram` 段或 `signals.vram_pressure_frames > 0` 时，用该段的 `start_ns`/`end_ns` 调用 `gpu.vram.summary`，按类别说明显存构成与增长；再用 `gpu.vram.resources`（默认取 DXGI 峰值时刻）和 `gpu.vram.resource_growth`（该段 `start_ns`/`end_ns`）点名具体资源，然后决定是否进入 Catalog 资源级分析。
- 录制带有使用率曲线时（`plot.list` 能列出 `CPU.System.Percent`，名称与口径见 [domain-playbooks.md](domain-playbooks.md) 的“使用率曲线”），对主导段和推荐窗口用 `plot.statistics`（`start_ns`/`end_ns`）取机器与游戏的 CPU 占用、各逻辑处理器占用、`GPU.Process.3D.Percent` 和内存曲线，作为分诊推断的旁证：`gpu` 段的 3D 引擎占用应接近饱和；`cpu_*` 段的 3D 占用明显不满时支持 CPU 侧瓶颈；机器 CPU 远高于游戏 CPU 时有其他进程争用，须写入测量环境说明。曲线是 100 ms 平均值，只描述秒级窗口，不能代替逐帧证据。

## 4. 选择窗口

默认使用 `scan_windows`，即全部推荐窗口。按以下情况调整，并把最终窗口及理由落盘为 `evidence/scan-windows.json`：

- 用户关注的场景、帧号或时间：加入对应窗口，原因记为 `UserFocus`。
- 占比 ≥ 5% 的瓶颈类别没有窗口覆盖时，补一个窗口：取该类别 Debt 最大的段内的 `window_seconds` 区间。
- 窗口总长度建议不超过 60 s，最多 64 个窗口。重叠窗口由 Query 合并。

## 5. 窗口扫描

```text
tracy_scan(operation="start", trace_id, profile, windows=[{start_ns, end_ns}, ...])
```

- `start` 和 `status` 的响应会回显规范化后的 `windows`。窗口扫描有自己的 scan identity 和中立聚合，与全量扫描及其他窗口集合互不复用。
- 扫描范围：完全落在窗口内的帧（连续 FrameSet 另加一帧收尾），以及与窗口相交的 CPU Zone、内存、I/O、采样和锁事件。GPU Zone、Job、Relation 和图形记录另含窗口后 250 ms 的余量。
- 定义类数据不受窗口限制：线程、源码位置、GPU Context、脚本帧、Callsite 和 GPU Catalog。
- 之后的生命周期、分页、候选导入、优先级分流、深查和报告与全量扫描相同，见 [query-native-scan-workflow.md](query-native-scan-workflow.md)。
- 窗口外仅有分诊信息。补查新窗口时启动新的窗口扫描，不要改写已完成的扫描。

## 6. 报告要求

报告必须明确：

- 分析覆盖：窗口列表、窗口总时长占录制的比例、每个窗口的选择原因。
- 全程概况：分诊的 `frame_time`、`limiters` 表和尖峰簇摘要，标为"分诊推断（全程）"。
- 结论范围：候选、Debt 和优先级只代表窗口内。窗口外的问题只有分诊证据，不进入 P0/P1。
- 分诊标签与窗口扫描证据矛盾时，以窗口扫描及帧证据为准，并记录分诊的偏差。

## 7. 失败处理

| 情况 | 动作 |
|---|---|
| 分诊返回 `INCOMPLETE_DATA`（没有完整帧） | 停止，记录输入质量问题 |
| 分诊 `missing_evidence` 非空 | 继续；相关类别只写假设，并在报告中列出缺失的信号 |
| 窗口扫描暂停为 `disk_budget`、`workspace_budget` 或 `process_memory_budget` | 释放空间或资源后，对同一 scan_id 调用 resume；不要缩小窗口重来来掩盖问题 |
| 窗口扫描失败 | 保存错误；可以缩短窗口重试一次，并在报告中记录 |
