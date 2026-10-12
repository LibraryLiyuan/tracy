# QSCAN-3 Neutral Aggregate Store 阶段验收

日期：2026-09-06  
状态：Passed

## 1. 身份

- Tracy worktree：`C:\CodeProjects\GodotProjects\tracy-0.13.1-ai-query-scan`
- 分支：`feature/JNTracy-AI-QueryScan`
- 阶段输入 HEAD：`b00344a7f072e7cd503190fc8a257e6ebbdf34c1`
- Query schema：`1.35.0`
- Native Scan API：`1`
- Neutral Aggregate schema：`1`
- Scan algorithm：`native-scan-v1`

阶段构建产物：

- `tracy-query.exe` SHA-256：`168FC24BFAA44D0190390A9554265F354AA58E177ED1D83BBEB7A59923D7472F`
- `neutral-aggregate-v1.schema.json` SHA-256：`EB9B7D2F055EA60BCA9900C4ACD75E46107ABAAE3F8FED5C021E147EBB505AB9`

## 2. 本阶段完成内容

1. 新增 config-independent Neutral Aggregate identity：
   - Trace 强身份；
   - Query 可执行文件 SHA-256；
   - Query schema；
   - Scan algorithm；
   - Aggregate schema。
2. Aggregate 路径固定为：
   `<analysis-cache-root>/<trace-strong-id>/native-scan-v1`。
3. 新增严格 JSON `aggregate-manifest.json`，包含：
   - schema/algorithm/trace/aggregate identity；
   - domain count 与 checksum；
   - quality；
   - generation、LRU、pin/report reference；
   - immutable run 清单。
4. 新增 immutable binary run：header、record count、payload size、SHA-256、临时文件和原子 rename。
5. 新增可恢复 checkpoint：stage、cursor、输入事件、输出 run、内存、磁盘和进度。
6. 复用经过 N30 验证的单 writer lease：PID、process creation time、heartbeat、stale lease recovery。
7. 新增 cache LRU 清理，明确保护 active、building/cancelled、pinned 和 report-referenced 条目。
8. 路径逃逸、identity mismatch、未完成 Aggregate 和损坏 run 都不能被复用。

Policy/Profile identity 不参与 Neutral Aggregate identity，因此只修改预算或候选规则时可以复用同一份完成的中立聚合；改变 Query 二进制、算法或 schema 时必须重建。

## 3. 测试证据

定向测试：

```text
tracy-query-deterministic-scan-contract  Passed  0.26 s
tracy-neutral-aggregate-store            Passed  0.08 s
```

完整 Debug 回归：

```text
11/11 Passed
Total Test Time: 10.44 s
```

覆盖：

- JSON manifest round-trip；
- domain/schema storage contract；
- frame run 与 string dictionary run round-trip；
- CancelledResumable checkpoint 与恢复信息；
- `.tmp` 未提交文件不进入 manifest；
- writer lease 互斥、heartbeat 和释放后重获；
- active/pinned/report reference LRU 保护；
- payload SHA-256 损坏拒绝；
-在显式更新 checksum 后继续验证 header，损坏 header 仍被拒绝；
-相对路径逃逸拒绝；
-Query/Session/GPU Analysis 既有回归无退化。

## 4. 构建基础设施观察

一次“同一命令请求多个共享静态库目标”的 MSBuild 执行在链接 `tracy-query-contract-tests` 时瞬时报告 `LNK1236`。证据表明：

- 同一个 `TracyQueryProtocol.lib` 已成功链接 `tracy-query.exe`；
-不修改任何源码或 object，改为单目标、单并发后立即成功；
-随后串行 11 项 CTest 全部通过。

结论：这是多目标 MSBuild 对共享静态库的瞬时读取竞争，不是 QSCAN-3 产品逻辑或确定性 COFF 损坏。后续阶段采用“逐目标构建 + CTest 串行带超时”，避免重复消耗。

## 5. 尚未在 QSCAN-3 执行的内容

- Query 扫描编排尚未调用该 Store；在 QSCAN-8 接入。
- 8/12/16 GiB 运行时内存门禁和 `RESOURCE_LIMIT_RESUMABLE` 由扫描编排实现；本阶段已提供 checkpoint 和状态存储原语。
- Domain scanner 尚未生成真实 run；QSCAN-4～QSCAN-7 实现。
- 未运行真实大型 Trace；QSCAN-9 统一执行 Oracle 与大 Trace 门禁。

以上均是计划内后续阶段，不属于本阶段失败项。
