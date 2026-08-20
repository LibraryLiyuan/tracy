# JN Tracy StreamConvert Offline 优化验收记录

## SC.0 隔离与基线

- 实施仓库：`C:\CodeProjects\GodotProjects\tracy-stream-convert-opt`
- 分支：`feature/JNTracy-StreamConvert-Performance`
- 起始提交：`f6a6db8ce05b72e2be57686aae3efe6e23e97a45`
- 原仓库：`C:\CodeProjects\GodotProjects\tracy-0.13.1`，保持只读。
- 压力输入：`Manual-HighEvidence-20260820-160336.stream-convert-opt.tracy-stream`
- 输入大小：`1,796,451,880` 字节。
- 输入 SHA-256：`697060EB63A1AA8B4F4057CC8644698242836FE489DC5BF12E0B6953DB747842`。
- 实际协议：当前源码与输入均为 Protocol 88；计划中的 Protocol 84 已按实际构建身份纠正。
- 事件扫描：14,511 个压缩帧、206,023,026 个 protocol events、295,352 个 server records。

旧 localhost/逐事件构建路径在压力文件上运行超过 10 分钟仍未进入 Write，已按阶段门禁停止；输入未修改，未发布不完整输出。

## SC.1 Offline transport 与首轮正确性

- Offline Worker 不监听端口，Client record 通过有界内存邮箱传入。
- Server transcript 在进程内逐 record 匹配并验证完整消费。
- 63,306,569 字节 Protocol 88 快速样本：Offline 与 Legacy 均消费 339 个 Client records 和 10,037 个 Server records。
- 两份输出均通过 `tracy-query --doctor`。
- `trace.overview` 全部公开计数一致；其中 `gpu_reference_uses=305068`。

## SC.2/SC.3 首个阻断热点

采样定位到 `JnGpuReferenceSetUse`：旧逻辑在每个集合事件前执行
`reserve(current_size + set_size)`，导致已展开引用在每个事件中被反复搬迁，形成近似 O(N²) 的构建成本。

修复严格限制在 `Worker::Mode::OfflineConvert`：容量不足时按 1.5 倍几何增长，事件展开顺序和字段不变；Full/ProtocolOnly 路径保持原行为。

快速样本结果（RelWithDebInfo、含诊断采样）：

| 指标 | 修复前 | 修复后 |
|---|---:|---:|
| Replay | 11.725 s | 0.374 s |
| 总转换 | 12.437 s | 0.956 s |
| GPU reference uses | 305,068 | 305,068 |

压力文件首轮结果（RelWithDebInfo、含诊断采样）：

| 阶段 | 时间 |
|---|---:|
| Scan | 9.082 s |
| Analysis | 0.313 s |
| Replay/Build | 22.587 s |
| Write | 5.763 s |
| Validate | 4.259 s |
| Snapshot map | 0.003 s |
| Total | 42.011 s |

输出大小 `762,221,363` 字节，并通过 Query doctor。公开计数包括：

- CPU zones：17,786,666
- GPU zones：2,308,721
- Job stages：21,176,579
- Samples：6,588,379
- Context switches：3,575,257
- GPU reference passes：1,693,641
- GPU reference uses：42,702,705
- Frame images：64

结论：性能已经进入三分钟进取目标，但“转换完成”仍需后续精确语义比较、正式 CLI、进度/取消/资源保护、Release 三次中位数和批量语料门禁。

## SC.2 有界执行与确定性

当前 Protocol 88 的 Client 压缩帧使用 `LZ4_decompress_safe_continue`，后一帧依赖前一帧的解码字典。因此不能在不修改采集协议的情况下把同一会话的 LZ4 帧并行解码；强行并行会破坏数据语义。SC.2 采用以下边界：

- journal scan 建立稳定 `record.sequence` 索引；Client 状态事件严格按原顺序提交。
- Client 输入采用单记录有界 mailbox，避免全量 payload 副本。
- Server transcript 使用进程内匹配、消费位点和周期性 compact，取消逐 record 的前部搬移。
- `--threads auto` 固定为 `max(1, logicalProcessorCount-1)`；用于可独立的输出流压缩和 Tracy 后台任务，进程优先级保持 Normal。
- 内存压力门禁为进程 commit 60% 物理内存、系统可用内存下限 `max(8 GiB, 15%)` 和系统 commit 85%；触发时 fail-open 停止转换，不丢事件、不发布输出。
- 同一快速输入分别用 1 线程和 27 线程转换；MCP 规范化比较的 Frame、GPU Memory、Resource Graph、Job、I/O、Script、FrameImage 及质量域哈希全部一致。
- 两种线程数均精确报告 `7,076,249` protocol events，与 `tracy-stream-inspect` 一致。

输出字节数不同是 Zstd 多流分块的内部表示差异，不是数据语义差异；不得用压缩文件字节相等替代语义比较。

## SC.3 高频域与 Worker 热点结论

压力文件的事件构成为：Sampling `6,588,379`、Context Switch `3,575,257`、Job Stage `21,176,579`、GPU Reference Use `42,702,705`。在修复 ResourceSet 的 O(N²) 扩容后，诊断采样中 Sampling、Context Switch、Callstack 均不再是阻断热点，单域估算只占数秒量级；继续改变其存储结构会扩大 Full/live Worker 回归面，却不能改善已经低于一分钟的总时长。

本阶段保留 Tracy 现有调用栈/线程字典、批量容器和后台派生分析，并使用 `NO_STATISTICS=ON` 避免转换期重复统计。诊断逐事件计时只在开发构建显式 `--diagnostics` 时启用，正式工具没有该采样开销。

## SC.4 CLI、写盘与失败行为

压力文件正式参数首轮（开发 Release 构建，不启用 diagnostics）：

| Preset | Scan | Replay/Build | Write | Validate | Total | 输出大小 |
|---|---:|---:|---:|---:|---:|---:|
| fast / 27 threads | 9.089 s | 23.912 s | 2.272 s | 3.406 s | 39.033 s | 769,632,903 B |
| legacy / 4 streams | 9.303 s | 21.871 s | 5.538 s | 约 3.9 s | 40.613 s | 约 736,947,702 B payload |

结论：fast 写盘比 legacy 快约 59%，文件仅大约 4.4%，满足 `≤125%` 门禁。

失败/恢复验证：

| 场景 | 结果 |
|---|---|
| `--disk-budget 1` | 构建 Worker 前退出，exit 3，无输出 |
| 协作取消 | exit 130，无最终文件和残留临时输出；有效 scan cache 保留 |
| 取消后重试 | 命中 scan cache，转换成功后删除本输入 cache |
| 注入 Validate 前失败并覆盖既有输出 | exit 99；既有输出 SHA-256 不变 |
| `--keep-failed-output` | 仅保留明确命名的 `.converting`，验证后已定点清理 |
| 截去最后 128 B，默认模式 | 恢复至最后有效 commit，忽略 77 B 尾部并成功发布；报告 `capture_complete=false` |
| 同一截断输入加 `--require-clean-end` | exit 2，无输出 |
| Protocol 83 输入 | 明确拒绝，exit 2，无输出 |

正式输出先写入同卷 `.tracy.converting`，重新打开验证后再原子发布 `.tracy`，最后更新 snapshot map。

## SC.5 语义比较状态

- 1 线程与 27 线程快速样本通过 MCP 规范化分域比较，报告：`sc2-threads1-vs-auto.semantic.json`。
- fast 与 balanced 快速样本通过同一比较，所有公开域哈希一致。
- 使用另一次 Protocol 88 capture 做负向测试，比较器正确失败；差异覆盖 overview、capture context/coverage、GPU Memory、Resource Graph、Job、I/O、Script 和 FrameImage。
- 当前 capture 自身缺少完整 build identity，`compare.normalized` 按既有契约拒绝性能配对；脚本只在两侧唯一 hard failure 都是同一个预存 `identity.complete=false` 时记录 `skipped_preexisting_capture_identity_incomplete`，不能伪装为已执行。
- `tracy-trace-compare` 只比较 Worker 重新序列化后的内部表示，已明确标为 diagnostic。内部 ID、哈希表遍历和压缩布局可不同，因此它不能替代 MCP 语义门禁。

一次大型 Query 索引计时与用户已有的长期 Query 分析进程并发，超过 600 秒后停止；该结果环境不洁净，标记为无效测量，未终止或干扰用户进程，也不计为 converter 失败。最终索引性能需在无并发 Query 的窗口复测。

## SC.6 Release 前正确性阻断与修复

首次三轮 clean Release 性能运行得到 `42.976 / 42.332 / 41.470 s`，但暂缓签字，因为报告的 `206,023,276` protocol events 比 Inspector 的 `206,023,026` 多 250。

逐层核对结果：

- journal Client records：输入/发布均为 `14,514`。
- 压缩帧：输入/发布/Worker 处理均为 `14,511`。
- 因此不存在 record 或压缩帧重复。
- 根因是 Protocol 88 的 `JnGpuReferenceSetDefinition` 未加入 `DispatchProtocolDrain` 的定义响应白名单。
- drain 兜底 `SkipProtocolEvent` 同时遗漏该事件的 32 位 payload 长度规则，错误按 16 位长度跳过，剩余 payload 被解释成 250 个伪事件。

修复：

1. 将 `JnGpuReferenceSetDefinition` 作为 drain 阶段必须保留、处理的定义事件。
2. 兜底跳过路径按 32 位长度安全前进。
3. converter 增加发布 record、压缩帧、Worker 帧和离线解码事件计数，覆盖不完整/重复重放门禁。

修复后同一压力输入：

| 项目 | 数值 |
|---|---:|
| Inspector protocol events | 206,023,026 |
| Offline decoded events | 206,023,026 |
| Published Client records | 14,514 / 14,514 |
| Published compressed frames | 14,511 / 14,511 |
| Worker compressed frames | 14,511 |
| GPU reference uses | 42,702,705 |

该轮总时间 `32.013 s`（显式 `--no-cache` 的开发 Release），并通过临时输出重开验证。最终 clean Release 三轮基准必须在合入该修复后重新执行；修复前的三轮仅保留为性能参考，不作为最终正确性门禁。
