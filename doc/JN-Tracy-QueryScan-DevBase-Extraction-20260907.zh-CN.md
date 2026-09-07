# QueryScan / SKILL 脱离 Session 的功能提取记录

日期：2026-09-07。范围：功能迁移与独立构建验证，不是完整 AI 分析流程验收，也不是新的 Query 缓存开发。

## 1. 交付位置与来源

| 项目 | 值 |
|---|---|
| 新分支 | `feature/JNTracy-AI-QueryScan-DevBase` |
| 新工作区 | `C:\CodeProjects\GodotProjects\tracy-0.13.1-ai-query-scan-dev` |
| dev 固定基线 | `9abafc1067d991d4373c54b912490e373f716283` |
| 原 QueryScan 分支 | `feature/JNTracy-AI-QueryScan` |
| 来源 HEAD | `fd4388533e1c6b31074dfa1c46aa3d73ff1ec591` |
| 来源范围 | QSCAN 提交及提取前 24 个 tracked dirty 文件、2 个 untracked 头文件 |
| Skill 来源 | Documents 下 `skill-draft/diagnose-jn-unity-tracy` 草稿 |
| 提取后 Skill | 本工作区 `skills/diagnose-jn-unity-tracy` |
| 本地完整记录 | `C:\Users\Admin\Documents\JN-Unity-T3\QueryScan-DevBase-Extraction-20260907` |

源代码/草稿快照共 173 个文件，另保存必要公共组件与独立文件哈希清单。原 QueryScan、N29、N30、dev 和另一个 QueryAnalysisCache 工作区没有被切换、覆盖、合并或推送。安装目录中的 Skill 没有更新。

不是把 Session 分支合并到 dev：从固定 dev 建立独立工作区，用白名单补丁提取 QueryScan，再适配最小公共接口。文本冲突只发生在新工作区。

## 2. 保留与排除

| 保留 | 排除 |
|---|---|
| Query 1.35 的 `tracy_scan` 12 个 operation | N30 Session Store / Canonical |
| Frame/CPU、GPU/Job/Managed、Memory/I/O/Sampling/Telemetry 扫描 | Session converter/exporter |
| 精确统计、Frame/Window/Global 候选入口、用户关注 | SessionTraceSource / Session Index |
| `native-scan-v2`、`candidate-policy-v3`、最新本地内存修复 | Profiler Session 后端 |
| 中立聚合、持久扫描状态、取消/恢复、缓存身份校验 | N29 磁盘 GPU sidecar reader/store |
| 质量附录不进入 P0/候选，全部 selected 必须调查 | 三十分钟 Session 扫描与验收 |
| Evidence/Source 回执、状态机、报告门禁与现有测试 | 新的 Query 分块缓存架构 |

普通 `.tracy` 是此发行版的 Skill 分析输入。录制和普通 `.tracy-stream` 转换入口继续沿用 dev；本次没有修改 capture、converter、server、public protocol 或 profiler 源码。

`TracySessionManager`、`trace_session_id` 是 dev 原有的 MCP Trace 句柄管理，不是 N30 Session Store，必须保留。捕获协议中的 `SessionEnd` 也不属于 N30 文件格式。

## 3. 最小公共依赖处理

| 组件 | 提取后的形式与理由 |
|---|---|
| 外部排序 | `TracyAnalysisExternalSort`：固定宽度 pair、磁盘归并；无 Session schema |
| Writer lease | `TracyAnalysisWriterLease`：PID/创建时间/心跳/原子替换；无 Canonical、CURRENT 或 Session manifest |
| Windows 长路径 | `TracyAnalysisIoPath`：通用文件路径包装 |
| GPU 扫描 DTO | `TracyGpuScanTypes`：仅资源/范围投影，不引入 sidecar reader |
| TraceSource | 回移扫描需要的 Count/Scan 分页接口、CPU 时间有效性字段；保留 Snapshot/Segment，不新增 Session kind |
| GPU 内存归一化 | 回移扫描实际消费的 typed records、generation/name 解析及 catalog-only 路径，避免 allocation 扫描强建完整 Pass snapshot |
| Hash | 通用增量 SHA 与原有文件身份功能，无 Session 身份模型 |

GPU 归一化代码并非 dev 字节级原样：独立复用了扫描需要的公共计算/DTO 改进。其小型内存缓存 schema 随 typed records 由 1 升为 2，旧缓存会拒绝复用并重建；录制协议、Trace 文件格式和原始数据不变。没有引入 N29 Raw Sidecar、N30 Canonical、Session Shard 或 Session Query 后端。

## 4. 迁移中发现与处理

1. 12 个文件的三方补丁冲突集中在构建清单、TraceSource、Query registry/schema 和测试。保留 dev 的非 Session 分支，只接扫描入口。
2. 扫描器需要 `CpuZoneDto.timingValid/timingInvalidReason`；补齐同语义字段，没有通过跳过质量校验解决编译问题。
3. 原测试夹具仍写 `native-scan-v1`，最新实现只接受 v2。先复现失败，再将正常夹具更新到当前算法；保留 v1 被拒绝的测试，未放宽缓存校验。
4. Skill 测试依赖开发机私有 YAML/JSON。改成发布示例和临时 fixture；新增 tests package 与发行边界测试。没有把私有路径配置提交进发行版。
5. 未调整扫描内存上限、Top/预算策略、统计口径或 selected 调查义务。

## 5. 验证结果

| 验证 | 结果 |
|---|---|
| 全部 Query CTest | 15/15 Passed，43.65 秒 |
| Skill Python | 58/58 Passed |
| Skill PowerShell | 结构、AST、JSON、旧脚本 synthetic、恢复、报告测试 Passed |
| Skill 安装/升级 | 临时目录首次安装、排除产物、保留本机配置 Passed；未部署到已安装 Skill |
| Skill 基础校验 | `quick_validate.py` Passed（使用 Skill 自带 PyYAML） |
| N27 Query/MCP 静态契约 | Passed |
| 来源文件保护 | 173 文件 SHA-256 全一致，来源/dev HEAD 未变化 |
| 构建依赖边界 | TracyAnalysis/TracyQueryProtocol 不含 Session Store/TraceSource 或 GPU sidecar Store |
| dev 非 Query 路径 | capture/profiler/server/public 无 diff |

真实输入是已有短 Trace：

```text
C:\Users\Admin\Documents\JN-Unity-T3\QSCAN13-Short-Skill-Acceptance-20260907\oracle-existing\
GPUCatalogOff-HighEvidence-20260903-202313.tracy
SHA-256: da5495e1bc5d85962bb0f9bdb2cabdc41e35c51ea0f1576a9eab702f36c15211
```

分别用提取版和原版常驻 MCP 打开一次，只验证 initialize/tools、Profile 校验、Frame get、Catalog status 和 close，未调用全量 `scan.start`。提取版约 11.61 秒、原版约 9.98 秒；不是性能 A/B，不能由这两个数字推导优化收益。进程 private peak 均约 6.37 GB，300 秒/16 GiB 保护未触发。

| Player.Frame | duration_ns | 对比结果 |
|---:|---:|---|
| 290 | 11529099 | begin/end/duration/complete/ref 完全一致 |
| 400 | 51846762 | 完全一致 |
| 540 | 22786118 | 完全一致 |

Catalog 的 counts、generation、quality 和失效原因一致。输入原有 `invalid_core_gap` / `unresolved_count=15366` 被忠实保留，没有伪装修复。原版额外的 `analysis_sidecar` 字段在此 dev 发行版中按范围移除；它不是 GPU 资源事实。详细对比见 `mcp-parity.json`。

## 6. 工具与使用边界

新 Query：

```text
C:\CodeProjects\GodotProjects\tracy-0.13.1-ai-query-scan-dev\build-query-devbase\Release\tracy-query.exe
SHA-256: d177e7b03696c311ab755ef9d47af9054e0f0d2b30cef0c5d645b8258ab0961b
```

构建发生于提取修改尚未提交的工作树；记录的 dev/source 基线和二进制 SHA 均保留，不能仅凭二进制内旧 Git 基线字段判断其全部内容。构建目录和依赖缓存独立，不复用另一工作区的编译产物。第三方源码缓存为只读复制来源；目标 cache 后续由自身 CMake 使用。

此分支可以作为后续 Query 缓存迭代的候选基础。先确认它的提交/分支，再创建后续开发分支；不要继续从 Session 开发线取整分支。

尚未完成、不得误报为完成：

- FW5 真实全量扫描及 FW6 最终 AI 报告验收。
- 之前大规模扫描在内存上限处中止的问题；本次仅保留已写的修复，没有通过大扫描证明问题消失。
- 新分块缓存、降低峰值内存、全量真实候选差分与全部候选调查。
- 新 Profiler 构建/GUI 验收、安装版 Skill 发布、dev 合并或远程推送。

## 7. 回退与证据

无需回退原分支：它们未改动。保留提取分支即可隔离新代码，不使用 reset/revert 覆盖用户工作。

本地记录目录包含 source-snapshot、selected-qscan.patch、independent-file-manifest.json、构建日志、CTest、Skill 回归日志、MCP 完整请求/响应、数值差分和 extraction-boundary-verification.json。未完成的大扫描缓存不作为此版恢复/验收基线。
