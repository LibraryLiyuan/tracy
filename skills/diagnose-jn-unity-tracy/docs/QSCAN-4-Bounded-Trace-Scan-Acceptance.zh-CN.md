# QSCAN-4 Worker / Session 有界扫描源验收记录

## 1. 结论

状态：**Passed（阶段轻量门禁）**。

QSCAN-4 已建立 Query 原生确定性扫描所需的统一有界游标，且生产版 `tracy-query.exe` 完整编译通过。扫描器不会回退到 `TraceSource` 中可能物化完整 `vector` 的旧接口；N30 Session 的 Job、Relation、Runtime、I/O、Gfx、Callsite 与 GPU Catalog 均直接使用磁盘 Reader 或 sidecar page。

真实大 Trace 的 Worker/Session 全域数值差分与内存曲线仍按总计划放在 QSCAN-13 集中验收。本阶段通过受控 Snapshot/Session Oracle 验证接口语义，并通过现有 Session/Query 全套回归测试验证生产适配。

## 2. 实现范围

- 新增 `NativeBoundedTraceSource` 显式能力契约；未声明该契约的 Source 会被扫描器拒绝。
- 新增 `GpuCatalogBoundedScanSource`，禁止 GPU 扫描回退到完整 `GetGpuCatalogData()`。
- 新增 24 个扫描域及类型安全批次：Frame、CPU/GPU Zone、Job、Relation、Memory、Sampling、Context Switch、Runtime、Script、I/O、Gfx、Message、Plot、Lock、CPU Usage、Callsite、GPU Resource/Allocation/Pass/Range。
- 游标固定记录 schema、domain、source kind、revision、时间范围、ordinal、最后时间、稳定实体和校验 token。
- 单次请求硬上限为 65,536 条。
- Worker、Indexed、Segment、N29 sidecar 与 N30 Session 路径均接入生产实现。
- Session Callsite 增加直接切片接口；GPU Catalog 使用 immutable page 按 offset/limit 读取。

## 3. 正确性门禁

| 检查项 | 结果 |
|---|---|
| Snapshot 与 Session 受控事实分页结果 | 100% 一致 |
| 分页边界重复/遗漏 | 0 |
| 旧 full-vector getter 调用 | 0 |
| 超过批次上限 | 明确拒绝 |
| 游标字段或 token 被篡改 | 明确拒绝 |
| Source revision 改变后继续游标 | 明确拒绝 |
| `.building` / incomplete Session | 明确拒绝 |
| 非 Ready Source | 明确拒绝 |
| 不支持有界契约的 Source | 明确拒绝 |
| 完整 `tracy-query.exe` 生产链接 | Passed |

## 4. 测试记录

构建目录：

```text
C:\CodeProjects\GodotProjects\tracy-0.13.1-ai-query-scan\build-query-scan
```

执行：

```powershell
cmake --build build-query-scan --config Release --parallel 1
ctest --test-dir build-query-scan -C Release --output-on-failure -j 1
```

结果：

```text
12/12 Passed
Total Test time: 6.12 sec
```

覆盖测试包括 Query contract、Query 1.35 deterministic contract、analysis profile、neutral aggregate store、bounded cursor、GPU analysis、Session store/inventory、MCP transcript、version/doctor 与 N29 static gate。

## 5. 测试过程中的非代码故障

首次直接运行 CTest 时出现 1 个断言失败和 7 个 Not Run。根因不是本阶段实现：当时只构建了 `tracy-query` 与新游标测试，旧 `tracy-query-contract-tests.exe` 仍内嵌 Query 1.34，而源码 schema 已为 1.35；其余目标尚未生成。单并发构建完整测试集合后，未修改业务代码即全部通过。

这也确认后续阶段不得在测试二进制未同步时把 CTest 结果解释为功能回归。

## 6. 已知边界与后续验收

- Worker 是短 Trace/差分 Oracle 路径；其 GPU Catalog 首次访问仍需从已驻留 Worker 事实建立归一化 snapshot。长 Session 路径不执行该操作，直接分页读取 N30/N29 存储。
- Worker 的 Job/I/O 分页保持有界输出，但基于已驻留 Worker 事实筛选；长录制的正式性能路径是 Session Reader。
- 真正的 G05 Session 与传统 Worker 全域 100% 差分、分页内存曲线和 cursor resume 故障注入，在 QSCAN-13 集中验收完成。

## 7. 阶段身份

分支：

```text
feature/JNTracy-AI-QueryScan
```

阶段基线：

```text
816411f4 QSCAN-3 Add atomic neutral aggregate store and resume state
```

目标提交：

```text
QSCAN-4 Add bounded Worker and Session scan cursors
```
