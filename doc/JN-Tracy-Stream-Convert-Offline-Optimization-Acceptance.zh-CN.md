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
