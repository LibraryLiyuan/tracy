# QSCAN-8 精确统计、异常形态与 Neutral Audit 验收记录

## 1. 结论

状态：**Passed（阶段轻量门禁）**。

QSCAN-8 已把各扫描器产生的“签名 × Frame”中性事实转换成确定性的精确统计、异常形态、三类全量排名和域级完整性审计。该阶段不读取预算、不裁剪 Top N、不生成性能问题结论；同一份 Neutral Aggregate 可在 QSCAN-9 使用不同策略重复评估而无需重扫 Trace。

## 2. 精确统计语义

- 同一 `domain + signatureId + frameScope + frameIndex` 的多条输入先精确合并。
- 分别保存：
  - `perCompleteFrame`：完整 Frame 分母；签名未出现的完整 Frame 以隐式 0 参与统计。
  - `whenPresent`：只统计签名实际出现的 Frame。
- 输出 `count/zeroCount/total/min/max/mean/median/MAD/p50/p90/p95/p99`。
- 百分位使用排序后相邻值线性插值；偶数、奇数和稀疏序列遵守同一公式。
- 隐式 0 不按 Frame 数量写入磁盘；分位数与 MAD 在读取有序序列时确定性插入，因此不会把稀疏签名扩张为稠密数组。
- 持续时间为负、整数累计溢出或分母小于实际出现 Frame 数时，结果显式标为 non-exact，禁止发布为完整 Aggregate。
- 未使用 t-digest、HDR Histogram 或任何近似 Sketch。

## 3. 有界外部排序

- 精确值和绝对偏差都复用 N30 的 `SortTraceSessionUInt64Pairs` external merge。
- 排序缓冲由 `maximumBufferedValues` 硬限制，0 会规范化为 1。
- 测试以 8192 个显式值、317 个隐式 0、257 条缓冲执行多 run merge，并与独立内存 Oracle 逐字段一致。
- 另执行 64 组固定随机种子 corpus，覆盖空集合、全 0、重复值、奇偶长度、不同隐式 0 数和 1～7 条极小缓冲。
- 当前接口消费各域扫描器已经形成的 `SignatureFrameRun`；从 Trace 到这些输入事实的跨阶段断点/恢复由 QSCAN-3/QSCAN-4 的 Store 与 Cursor 承担，QSCAN-10 负责把二者编排为完整扫描事务。

## 4. 异常形态

中性算法固定为 `native-scan-v1` 的事实语义，不是项目预算：

```text
MAD > 0：value > median + 6 × MAD
MAD = 0：value > median
```

分类：

| 类型 | 中性判定 |
|---|---|
| `IsolatedSpike` | 只有一个异常 Frame |
| `RecurrentSpike` | 至少三个异常 Frame，且没有连续三帧 Burst |
| `BurstWindow` | 至少连续三个异常 Frame |
| `PersistentPressure` | 无异常尖峰、至少80%完整帧出现、median>0、MAD≤median的10% |

同时保存异常 Frame、原始值、相对 median 的 delta、最长连续长度，以及至少三个异常点且间隔完全一致时的周期。预算超限、业务显著性和候选晋级只在 QSCAN-9 判断。

## 5. 排名与树状 Zone 口径

每个数据域生成三套完整排名：

- `Inclusive`：含子节点时间，用于观察阶段/父树总体压力。
- `Exclusive`：扣除直接子区间并使用 scanner 已去重事实，用于定位真正执行热点。
- `Wait/Critical Path`：使用 `max(wait, criticalPath)` 排名，同时保留两种原始总量，避免重复累计同一等待链。

所有 ranking 在磁盘中完整保存，不在 Neutral 阶段截取 Top N。贡献率按相同排名全集计算，最后一项累计贡献固定为 1。逻辑 rollup 签名保留统计，但不混入物理 exclusive/dedup 排名，避免父子重复计数。

## 6. Neutral Audit 与发布

- 每个域核对：source input count、scanner consumed count、aggregate output count、input checksum、consumed checksum 和 quality 状态。
- 任一 count 差异按实际缺口累计；checksum、重复/非法审计、缺少域审计各产生明确 finding。
- 已知 `absent/invalid/unsupported` 域可以忠实记录，不伪装为空且完整；未报告的缺口阻止发布。
- 内容在稳定排序、规范 JSON 后生成 SHA-256；反转输入与分母顺序后 SHA 完全一致。
- 发布先写 generation 唯一的 immutable run，重新读取并逐字节验证，再原子替换 manifest。
- invalid 结果不会覆盖上一份 Complete manifest；测试确认拒绝发布后旧 generation 仍可复用。

## 7. Synthetic 门禁

| 检查项 | 结果 |
|---|---|
| 偶数 `{1,2,3,4}` | median/p50=2.5，p90=3.7，MAD=1，Passed |
| 奇数 `{1,2,3}` | median=2，MAD=1，Passed |
| 稀疏 `{10,20}+3×0` | count=5，median=0，p90=16，Passed |
| 64组固定随机 Oracle | 所有精确字段100%一致 |
| 8192值 external merge | 257条缓冲，多 run 合并，Oracle 100%一致 |
| 同一 Frame 重复 run | 合并为25；occurrence仍为3，Passed |
| Isolated/Recurrent/Burst/Persistent | 四类均准确 |
| 周期异常 | interval=2 Frames，Passed |
| 三套排名 | Inclusive/Exclusive/Wait-Critical顺序准确 |
| 输入逆序 | content SHA-256完全一致 |
| 域 count/checksum audit | 完整输入通过；少消费1条准确失败 |
| invalid 发布 | 被拒绝；上一Complete generation不受影响 |
| 最大排序缓冲 | fixture中≤2；external corpus固定257 |
| QSCAN-1～8 联合回归 | 9/9 Passed |

## 8. 测试记录

红灯阶段：测试目标按预期因 `TracyExactStatistics.hpp` 不存在而编译失败。

实现后执行：

```powershell
cmake --build build-query-scan --config Release `
  --target tracy-exact-statistics-tests -j 1

ctest --test-dir build-query-scan -C Release `
  -R '^(tracy-query-contract|tracy-query-deterministic-scan-contract|tracy-analysis-profile|tracy-neutral-aggregate-store|tracy-bounded-scan-cursor|tracy-frame-cpu-scanner|tracy-gpu-job-managed-scanner|tracy-memory-io-sampling-telemetry-scanner|tracy-exact-statistics)$' `
  --output-on-failure
```

结果：

```text
9/9 Passed
Total Test time: 2.23 sec
```

受控 QSCAN-8 测试二进制 SHA-256：

```text
F1DE1DAE026B363E3B1A1B632B4A0248695518861F86ED5AD29810F9BD71B0AA
```

## 9. 已知边界与后续门禁

- QSCAN-8 不使用主线程、显存或任何项目预算；Policy、候选数量和代表帧在 QSCAN-9。
- QSCAN-8 的中性异常形态是确定性数据特征，不等价于“必须修复的问题”。
- 当前阶段验证 external merge 正确性和缓冲上限；输入翻倍≤2.5倍、长 Trace 内存和墙钟属于 QSCAN-13 集中 corpus/真实 Trace 门禁，不能以本次小型单元测试冒充。
- interrupted scan 的事实恢复由已有 Cursor/Checkpoint 负责；本阶段保证从同一完整输入重算的内容 SHA 不依赖输入顺序，且只发布完整原子结果。QSCAN-10 将验证端到端 cancel/resume。
- 真实 Worker `.tracy`、N30 Session、Query/MCP 与人工 Profiler 对照仍在 QSCAN-12/QSCAN-13。

## 10. 阶段身份

基线：

```text
07d4d10d QSCAN-7 Add memory IO sampling and telemetry scanners
```

目标提交：

```text
QSCAN-8 Add exact statistics anomaly detection and neutral audit
```
