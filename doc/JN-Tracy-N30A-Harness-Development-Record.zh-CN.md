# N30A 验收器开发记录

日期：2026-09-07。分支：`feature/JNTracy-N30-LongTraceSessionStore`。

## 开发自测

此前 Python 完整自测 120 项通过；2026-09-07 追加控制台解码和锚点比较后，129 项通过，0 失败、0 错误。包含精确整数、Golden、日志、报告、局部运行不得宣称全量通过、进度落盘、取消、Windows Job Object 低内存限制、质量警告参与比较和依赖停止规则。

这些结果仅证明验收器自身，不能替代正式 N30A 验收。

## 已保留的合成开发运行

- `n30a-20260907-063208-58a9e670`：工具输出 schema 为十进制字符串，Python 解析器错误拒绝，记为 HARNESS_ERROR；已修正并加入测试。
- `n30a-20260907-063352-58a9e670`：68 个比较，67 PASS / 1 FAIL_PERFORMANCE。100k/200k/400k 均只有一个数据 Shard，未覆盖分片回收后的内存行为；4×/1× 峰值比约 2.764。保留原始失败，不据此修改门槛。
- `n30a-20260907-063723-6f45f123`：外部中断；最后记录位于 1× 派生索引，进程随后不存在，未形成阶段结论。

上述目录位于本机独立验收根目录的 `runs/`。未删除源文件、Session 或开发 scratch。

## 生成器修正

发现 LZ4 连续压缩引用了下一次会被重用的输入缓冲；改为独立压缩块。增加生成最大 Frame ID 与 Session 最大 Frame ID 的精确比较。密度基准提高到 1m/2m/4m，语义时间与 Journal 时间使用相同 100 µs 步长，要求至少两个数据 Shard。新语料需由独立构建产物正式验证。

## 正式运行前补齐

- 比较逐项输出 expected/actual/delta/status；原始日志纳入 SHA-256。
- 每 5 秒验收心跳；命令启动前保存 invocation。
- 锁定代码、工具、配置和输入身份；P9 重新检查 source SHA 与既有 Session 文件元数据。
- P5 重新执行只读 Canonical/Final Audit；Fresh 转换期已运行独立 GPU Verifier，其结果与 A7 和 Golden 比较。
- Converter 外层 18 GiB Job Object 护栏，独立于产品内部 16 GiB 限制。
- 局部阶段通过仍为 INCOMPLETE；依赖阻断不会误阻断允许的只读 A7 诊断。

正式 P0–P9 尚未完成，不应将本记录作为产品验收通过报告。

## P6 覆盖补齐与 P2 准备结果

- 新增只读 Native Anchor helper：固定首/中/末数据 Shard、代表帧、Resource ID、GPU peak、最大 allocation、最大 direct fan-out Pass、跨 Shard Resource 和 FrameImage。读取锁定 generation 的 immutable reader，不启动 Worker，不重读 source，不修改 Session。
- Python 保存两次 anchors 输出并比较 SHA-256；再比较公开 Query 的帧、资源、allocation、pass 正反向、Source/Stack、Job 和图片关联。全量真实性仍由 P3/P5 差分与审计负责，不能用这些定点查询替代全量审计。
- 分页扩展到 FrameImage、GPU Resource、Frame、Job、CPU Zone、Sample；每域三小页和一大页比较顺序、重复及遗漏。
- 既有源端退化仍显式保存。无法提供精确退化区间时不推测区间、不声称已经完整覆盖该区间。
- 工具链准备运行 `n30a-20260907-064741-947ba267`：构建命令全部成功，CTest 8 项全部成功；18 个比较中 17 PASS / 1 HARNESS_ERROR。失败项是 `--version` 退出码 0 但 stdout/stderr 为空。正式 `system.capabilities` 成功返回 envelope/data schema `1.34.0`。因此 P2 改用正式 JSON 响应做版本门禁，不修改 Query 产品代码，也不放宽版本要求；原失败日志永久保留。
- Windows 工具日志按 UTF-8 优先、系统代码页回退；按完整行解码避免拆断汉字。协调器控制台强制 UTF-8，原始子进程 stdout/stderr 字节不变。

## 只读 A7 Smoke：帧身份修正与独立查询超时

`anchor-smoke/` 在异常前已落盘 175 项 PASS，但运行未完成，不能记为阶段通过。

| 项目 | Native Reader 预期 | 公开 Query 实际 | 差值 | 结果 |
|---|---:|---:|---:|---|
| GPU physical peak bytes | 3,848,863,744 | 3,848,863,744 | 0 | PASS |
| GPU peak time ns | 1,859,905,005,679 | 1,859,905,005,679 | 0 | PASS |
| 最大 allocation bytes | 570,425,344 | 570,425,344 | 0 | PASS |
| Pass 239237 direct resource count | 394 | 394 | 0 | PASS |
| Pass 239237 referenced physical bytes | 685,465,600 | 685,465,600 | 0 | PASS |

### 验收器错误，不是 Session 丢帧

Catalog 的 `frame_id=617` 实际是 GPU domain 的局部 frame index；验收器错误地将它作为 packed FrameIdentity 输入 `frame.identity`。源记录明确给出 `domain_id=4, domain_index=617 → frame_id=562949953421313`。改为只使用显式 alias 关系，缺少或多义映射直接失败；不按相同数字或时间接近猜测。新增回归先失败，再实现并验证。Native helper 仅导出原有 alias 数据，不修改产品代码或录制数据。

### 查询超时，不是已证明的数据损坏

`frame-id-diagnostic/` 中 `job.search(limit=1)` 超过 60 秒无响应。最后进程指标：Read Bytes 35,725,564,726；峰值 Private Bytes 1,552,543,744；用户态 CPU 33.3125 秒、内核态 20.5 秒。进程已退出，无后台遗留。该单次诊断不是三次正式性能门禁，未获得结果，不能宣称 Job 数据正确或错误。

同一进程先前的 `frame.identity(limit=1)` 返回正确 alias；响应自报 cpu_ms=10691、max_cpu_ms=5000、partial=false。这是待核查的预算响应现象，不在本轮擅自修复。

验收器原先将所有 MCP 超时归为 `FAIL_CORRECTNESS`，已用真实子进程回归修正为 `McpTimeoutError` / `FAIL_PERFORMANCE`；损坏或缺项响应仍为 `FAIL_CORRECTNESS`。原始失败保留，禁止混淆两类结论。

源码只读排查发现：`TracyGpuAnalysisTraceSource.cpp::SessionJobReader()` 首次访问调用 `TraceSessionJobReader::Open()`；后者无条件执行 `ValidateRawStore()` 与 `AuditTraceSessionJobPagingDerived()`，对 raw、page、posting 文件做完整 SHA-256 和顺序审计后才分页。`job.search(limit=1)` 不能限制这些首次打开成本。该路径与高读字节现象一致，但尚未做阶段计时证明每部分耗时占比。修复方向应在保留身份及完整性保证的前提下分离全量审计与按需读取，而不是直接跳过校验；此轮不修改产品。

### 修正后复查

`anchor-alias-helper-build/` 增量构建退出码 0，耗时 369.734 秒，峰值进程树 Private Bytes 6,088,863,744。两次 Native Anchor 选择耗时 31.687 秒和 22.516 秒，SHA-256 均为 `f7ac9c020c3e159818a2131eff44fafc19706f34b3301535507c305e331787b4`。`anchor-alias-smoke/` 中 FrameIdentity 查询与 alias 回查均返回 562949953421313，差值 0；先前验收器 ID 空间错误已通过真实数据复查。

最终 Python 自测 134 项通过、0 失败/错误，耗时 1.957 秒。只读 Query smoke 的最终状态以 `anchor-alias-smoke/report/acceptance-result.json` 为准；未执行阶段不能因已有定点通过而补记为 PASS。

该 smoke 最终为 **178 比较 / 177 PASS / 1 FAIL_PERFORMANCE**。失败项为 `timeline.correlated_slice(ref=frame-identity:2000000000001,max_nodes=10000)` 超过 60 秒无响应（MCP request id=83）。查询进程 PID 67616 已由验收器按已验证进程身份结束，峰值 Private Bytes 4,259,414,016。没有继续 Job/FrameImage 后续步骤，没有自动修改产品或启动 fresh 30m。该结果说明当前跨域冷查询性能未过观察期限，不是证明 Session 数据不一致；正式三次性能测量及其余阶段尚未完成。

### 仍须明确的覆盖边界

- 第一个数据 Shard 没有选出语义 Frame 锚点；不得声称三个代表 Shard 的 Frame 全部覆盖。
- 跨 Shard Resource 候选 60437 没有 allocation，capacity=0；字段比较通过不代表正常资源全链通过。
- 最大 Resource ID 为 `Untracked GPU Resource`；保留源退化，不隐藏为完整分类资源。
- Native Reader 与公开 Query 的定点比较不能代替 P3 独立短 Trace 差分和 P5 全量审计。
- 正式 fresh 30m、P0–P9 全量验收仍未执行；发现产品问题后按设计停止依赖路径，不自动修复产品。
