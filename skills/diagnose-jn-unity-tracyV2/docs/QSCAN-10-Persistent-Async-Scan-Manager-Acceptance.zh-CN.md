# QSCAN-10 持久化异步 Scan Manager 验收记录

日期：2026-09-06  
开发分支：`feature/JNTracy-AI-QueryScan`  
阶段基线：`4e1352a2`（QSCAN-9）

## 实施结果

- 新增 `AnalysisScanManager`，扫描启动立即返回稳定 `scan_id`。
- 扫描状态、标准化 Profile、中性聚合、Policy Context 和 Candidate Manifest 全部落盘。
- 支持 `status/cancel/resume/close`；取消状态在 2 秒门槛内可见，并保存 resumable checkpoint。
- Query 进程重启后可发现完整结果；发现上次进程遗留的非终态扫描时转为 `cancelled_resumable`，不伪装为仍在运行。
- 同一 Trace、Query EXE、Query Schema 和扫描算法共用中性聚合；只改变 Profile/候选策略不会重新扫描 Trace。
- `analysis.scan.signatures` 与 `analysis.scan.candidates` 支持稳定游标、字段投影和精确过滤。
- QueryService 对 `analysis.scan.*` 使用独立调度路径，不持有旧的全局 Query cache mutex。
- MCP 新增专用 `tracy_scan` 工具，12 个操作直接映射持久扫描 API，不依赖仅存在于当前进程的 `tracy_job`。
- 输出继续受单响应 8 MiB 门禁约束。
- 默认协调器已串联 QSCAN-5～8 Scanner；中性按帧候选当前只纳入具有确定 Frame 归属的 CPU、物理 GPU 和 Job 数据。Managed、Memory、I/O、Sampling、Scheduling 与 Telemetry 均执行扫描和质量审计，其中容量证据进入候选策略；更深的候选详情由 QSCAN-11/12 补齐。

## 关键正确性修复

- 为 `NeutralStatisticsResult` 增加带 content SHA-256 复核的序列化/反序列化，禁止重启后读取被篡改的缓存。
- 修复 Candidate 查询对临时 JSON 子对象进行 range-for 导致的悬空引用。
- Windows QueryService 引入 EXE SHA-256 身份时显式解除 `FindResource` 宏污染。
- Scan worker 顶层捕获异常并持久化 `failed`，避免后台异常终止 Query 进程。
- `close` 只释放内存中的 TraceSource pin，不删除扫描证据。

## 自动化验收

完整 CTest：`18/18 passed`，总耗时 `8.04 s`。

重点场景：

- start 即时返回；
- 同 scan 重复 start 只有一个 executor；
- cancel 在 2 秒内进入 `cancelled_resumable`；
- resume 完成并发布；
- Query 重启后读取 summary/signature/candidate；
- Profile-only 变化复用中性聚合；
- 扫描运行期间普通 `trace.info` 小于 2 秒且不被扫描锁阻塞；
- MCP `tracy_scan` 契约与路由；
- 全部既有 Query、GPU、Session、Scanner 与 N29 静态回归。

## 构建身份

```text
tracy-analysis-scan-manager-tests.exe
SHA-256 DF0D8F3E60E32B7D7C238E5A7E067A40D209673C9EBA783CDA7274A8E566AE1A

tracy-query.exe
SHA-256 4DB0888E2DD875B6BB56FF4104846CEBC84489A8CECED7FCD2EFC10FD70104D4
```

## 阶段结论

QSCAN-10 的持久化、异步生命周期、重启复用、分页、MCP 路由和非阻塞门禁通过。真实大型 Trace 的取消粒度、内存/磁盘压力和结果完整性仍按计划在 QSCAN-13 集中验证，不能由本阶段的 synthetic 测试代替。
