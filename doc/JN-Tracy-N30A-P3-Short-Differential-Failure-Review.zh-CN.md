# N30A P3 短 Trace 双路径验收：失败项与证据说明

日期：2026-09-07。分支：`feature/JNTracy-N30-LongTraceSessionStore`。

执行代码：`e3bfec71273e64292b77864abd46757b50e62548`。这是运行后的补充解读，不改写原机器报告，也不代表产品修复。

## 1. 结论与停止位置

**P3 未通过。已停止依赖它的新 30 分钟转换及后续全量验收。**

两个转换器均成功退出；失败发生在后续双路径验收：

1. 完整 Worker Query 返回 20/24 个响应后，下一请求 `gpu.catalog.validation` 长时间未返回。连续 459.860 秒没有新输出后，按进程身份受控停止。
2. 从已经取得的 `trace.counts` 响应做 Python 精确比较，45 个字段中 29 PASS、16 FAIL。
3. 只读回查 Session manifest，确认部分失败来自 Query 字段未填充或单位不同；但 Job 底层索引计数仍有差异，尚不能宣称事件全部正确。

不能将所有差异解释成“录制丢数据”，也不能全解释为“显示问题”而跳过门禁。本轮未自动修复产品、重建产品或重试失败请求。

## 2. 已完成与未完成

| 范围 | 结果 | 边界 |
|---|---|---|
| Python 验收器自测 | 137/137 PASS，2.003 秒 | 只证明验收器回归测试 |
| P0 独立运行 | 21/21 PASS，另有 P9 2/2 PASS | 身份、输入、磁盘预检 |
| P2 独立运行 | 18/18 PASS，CTest 8/8，API 1.34.0 | 独立工具链 |
| P3 隔离运行 | FAILED | 转换成功，Query 不完整且计数不同 |
| 最后身份复查 | 10/10 PASS | 8 个工具与两份短输入 SHA 匹配 |
| 本轮 P9 | 2/2 PASS | 30 分钟 source SHA、既有 A7 Session 文件元数据不变 |
| Fresh 30m、全量审计、完整 Query、恢复与规模门禁 | 未继续执行 | 不能补记 PASS |

P0/P2/P3 是不同独立 Run，不拼成一轮全部通过。原 P3 报告主摘要为 14 项（12 PASS / 2 FAIL），逐项表另附 P9 两项 PASS，共 16 行。补充的 45 项计数、9 项 manifest、10 项身份比较单独保存，不覆盖原报告。

## 3. 输入、产物与成本

验收根目录：

```text
C:\Users\Admin\Documents\JN-Unity-T3\N30A-Full-Acceptance-20260906
```

P3 Run：

```text
short-oracle-isolated\runs\n30a-20260907-085424-34fb83c5
```

短输入：1,235,094,582 字节，SHA-256：

```text
42991418004dcc34cb367e59ef74cc45b9e4476cd80979f4402f8bab478ca809
```

输入先复制到本 Run 的 `work/P3/attempt-001/full-worker/input.tracy-stream`，两条路径都使用此副本。完整 Worker 使用已有 offline 模式，仍生成完整 `.tracy`，不是 Session 自证。snapshot-map 写在隔离目录，不触碰原捕获目录。

| 命令 | 墙钟秒 | 峰值 Private Bytes | 退出/结果 |
|---|---:|---:|---|
| 完整 Worker 转换 | 29.843 | 6,484,979,712 | exit 0，生成 `.tracy` |
| Session 转换 | 569.516 | 884,629,504 | exit 0，发布 Session |
| 完整 Worker Query 批处理 | 513.562 | 14,538,416,128 | 受控停止，20/24 响应 |
| Session 同一批 Query | 3.594 | 100,356,096 | exit 0，24/24 响应 |

Session generation 为 `n30-1788771312826076-37044`，有 28 个 Canonical shard、142,901,883 indexed records；独立 GPU verifier 完成。Indexed records 不直接等于协议事件数，禁止混用单位。

这是单次观测，不是多轮性能门禁，不线性预测 30 分钟文件耗时。Session 还做派生索引和审计，两个转换不是等工作量算法 A/B。其他任务可能影响墙钟；未测定各步骤独占 CPU 时间。

## 4. Python `trace.counts` 精确比较

预期 = 完整 Worker `.tracy` Query；实际 = Session Query；差值 = 实际 − 预期。严格相等，无容差，无静默排除。

**45 项：29 PASS / 16 FAIL。以下列出全部失败。**

| 字段 | 预期 | 实际 | 差值 | 结果 |
|---|---:|---:|---:|---|
| callstack_frames | 1,057 | 1,655 | +598 | FAIL |
| cpu_zones | 18,959,111 | 18,959,384 | +273 | FAIL |
| frames | 13,677 | 13,674 | −3 | FAIL |
| gfx_entities | 4,561,826 | 4,561,829 | +3 | FAIL |
| gfx_links | 7,714,457 | 7,714,458 | +1 | FAIL |
| gpu_reference_ends | 1,969,665 | 0 | −1,969,665 | FAIL |
| gpu_reference_passes | 1,969,665 | 1,969,666 | +1 | FAIL |
| gpu_reference_uses | 26,533,453 | 0 | −26,533,453 | FAIL |
| job_dependencies | 410,164 | 0 | −410,164 | FAIL |
| job_stages | 16,788,597 | 0 | −16,788,597 | FAIL |
| job_types | 24 | 0 | −24 | FAIL |
| jobs | 1,422,964 | 1,423,028 | +64 | FAIL |
| memory_pools | 10 | 9 | −1 | FAIL |
| plots | 81,645 | 48 | −81,597 | FAIL |
| relations | 9,061,438 | 9,061,439 | +1 | FAIL |
| threads | 25 | 29 | +4 | FAIL |

相等的非零字段包括 GPU Zones 2,633,381、FrameImage 75、Frame sets 3、correlated frame events 27,345、Memory events 251,655、I/O requests 3,647、I/O stages 7,419、Callsites 1,067、Callstack payloads 560、SourceLocations 1,093、Messages 9,128。完整 45 行见 Run 的 `partial-counts-differential/P3-Counts-Differential.zh-CN.md`。

Sampling、Context Switch 等两侧为 0，只证明这份短输入计数相同，不证明有数据时实现正确。

## 5. Manifest 回查：哪些原因已确认

`TracyGpuAnalysisTraceSource::GetTraceInfo()` 未填充 `jobTypes/jobDependencies/jobStages/gpuReferenceEnds`。默认 0 不能表示底层真的没有记录。

Session Job manifest 有 24 个 type、410,199 条 dependency、16,790,519 条 stage。但是它与 Worker 仍有差异，不能仅补 DTO 就宣布完成。

Python 回查 **9 项：3 PASS / 6 FAIL**：

| 比较对象 | 预期 | 实际 | 差值 | 结果 |
|---|---:|---:|---:|---|
| Session manifest → Query：job_types | 24 | 0 | −24 | FAIL |
| Session manifest → Query：job_dependencies | 410,199 | 0 | −410,199 | FAIL |
| Session manifest → Query：job_stages | 16,790,519 | 0 | −16,790,519 | FAIL |
| Worker → Session manifest：job_types | 24 | 24 | 0 | PASS |
| Worker → Session manifest：jobs/schedules | 1,422,964 | 1,423,028 | +64 | FAIL |
| Worker → Session manifest：dependencies | 410,164 | 410,199 | +35 | FAIL |
| Worker → Session manifest：stages | 16,788,597 | 16,790,519 | +1,922 | FAIL |
| 对齐为 Plot 数据点数 | 81,645 | 81,645 | 0 | PASS |
| 对齐为 Callstack frame address 数 | 1,057 | 1,057 | 0 | PASS |

Plot 根因：Worker `GetPlotCount()` 汇总用户 Plot 的数据点，Session 返回曲线条数 48。Callstack 根因：Worker 返回 frame address 数，Session 返回 inline frame 数 1,655。这两项是接口统计口径不一致；没有改返回值或从原 16 个失败中删除。

CPU Zones、Frame、Gfx、Job 非零差值、Memory pool、Relation、Thread 及其余 GPU Reference 差异需按源记录和时间边界定位。未经证明不把 −3 Frame 当作尾部 sentinel，不把 pool 差异当作空池忽略。尚未逐事件对账，不声明哪条路径丢失或重复。

## 6. 完整 Worker Query 为什么停止

输出第 20 条 `gpu.catalog.status` 后，下一请求为 `gpu.catalog.validation`。连续 459.860 秒无新输出，进程仍消耗资源；确认 PID 74536、创建时间、程序和本 Run 参数后只停止该进程。

不是 Query 打开阶段没进度，也不是操作系统内存崩溃。机器报告 `timed_out=false`：本次在批处理总保护期限前受控停止，退出码 4294967295 由此产生。缺失的四条响应不能记为空结果。

源码显示完整 Worker validation 为收集 reference pass IDs 调用 `GetGpuMemoryAttribution()`，后者扫描 CPU/GPU Zones、构建 Gfx 映射、匹配重复 GPU query ID 候选并构建全量归属数据。Session 分支使用固定 generation 已存 independent verifier 结果。两路径成本结构不同。

这是源码支持的排查方向，没有函数级采样/计时，不能把 459.860 秒全部归于某一函数或声称唯一热点。

原报告将非零 Query 退出码和缺响应统一归类 `FAIL_CORRECTNESS`；这两项本身只说明执行不完整，不单独证明数据损坏。另行取得的 16 项计数差异确实使精确差分门禁失败。原机器记录不覆盖，分类局限在此明示。

## 7. 证据与透明记录

本 Run 内：

```text
report/N30A-Full-Acceptance-Report.zh-CN.md
report/acceptance-result.json
comparisons.ndjson
partial-counts-differential/P3-Counts-Differential.zh-CN.md
partial-counts-differential/comparisons.ndjson
manifest-count-diagnostic/comparisons.ndjson
query-stall-diagnostic/observation.json
final-identity-review/comparisons.ndjson
final-identity-review/evidence-sha256.json
work/P3/attempt-001/query-full-worker/responses.ndjson
work/P3/attempt-001/query-session/responses.ndjson
```

辅助 Python 位于验收根目录：`compare-p3-counts.py`、`compare-p3-manifest-counts.py`、`measure-p3-query-stall.py`、`verify-p3-final-identities.py`。只分析现存证据，不改 Trace/Session。Manifest 诊断首次错将 Path 而非文本传入解析器，在任何比较前退出；修正实参后才生成 9 项结果，不是产品问题。

此前 27/27 PASS 的 `worker-replay-report-comparison` 只比较旧 Worker 与新 Worker，不比较 Session，不能覆盖本次失败。早期 TCP replay / snapshot-map 目录两次验收器错误及原日志保留。

每项打印名称、规则、预期、实际、差值和 PASS/FAIL；数值保留整数原值，不用四舍五入掩盖差异。

## 8. 下一步须评审的产品工作

以下尚未执行，也未放宽门禁：

1. 统一 `trace.counts` 字段契约，补齐 Session 未填充统计；分清 Plot point/series、Callstack address/inline frame。
2. 对非零差异按源 record ordinal、开放边界、生命周期做逐事件对账；保留源退化。先证明原因，再修转换或查询。
3. 让 Worker Catalog validation 只做证明正确性必需的工作，避免为校验 IDs 构建完整归属；必须有等价回归，不能简单跳过校验。
4. 前一轮 A7 `job.search` / `timeline.correlated_slice` 冷查询超时仍待处理；首次全文件校验不受分页 limit 限制的成本有源码证据，须独立测试。
5. 修复获批并完成后重新锁定代码、工具身份，重跑短 Trace P3；通过后恢复 fresh30m 及后续验收。不拿大文件反复试错。

没有修改 Unity、PackageRepo、Player、dev/N29 分支。未合并或推送 N30。原 stream 不删除、不覆盖。
