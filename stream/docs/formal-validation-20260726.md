# Tracy 流式传输正式验证报告（2026-07-26）

## 1. 验收结论

本轮对 `.tracy-stream` 的构建、写盘、实时读取、停止排空、异常恢复、严格
转换、历史格式兼容、Query/MCP、真实 Godot/TPS 和离线符号解析进行了正式
验收，不是只检查进程能否启动的冒烟测试。

结论为：**推荐工作流通过功能验收，可以合入并用于当前 Godot/TPS 开发与
自动化录制。** 推荐工作流是：

1. 游戏启动前设置 `TRACY_SYMBOL_OFFLINE_RESOLVE=1`；
2. 使用 ProtocolOnly `-j` 录制，保留默认 30 秒排空无进展看门狗；
3. 录制期间可直接用 Query/MCP 读取已提交 revision；
4. 结束后显式执行 `tracy-stream-convert` 生成标准 `.tracy`；
5. 需要完整 PDB 符号时，再执行 `tracy-update -r`。

该结论不等于“任何生产环境都零风险”。本轮最长真实连续录制为 60 秒，
没有完成关闭 Tracy/开启 Tracy 的主线程 p99、p999 对照，也没有用真实慢盘
做数小时压力测试。在线 DbgHelp 符号模式虽然已被看门狗限制为有界失败，
但不保证每次主动停止都能写出完整 `SessionEnd`。这些边界见第 9 节。

## 2. 仓库和环境

- Tracy 仓库：`C:\CodeProjects\GodotProjects\tracy-0.13.1`
- 分支：`feature-StructuredData`
- 本轮修改起点：`2717b2a439bde7c92fe75b56cbef0df2db7288fd`
- worktree：只有上述一个主工作树，没有并行工作树
- Godot：`godot.windows.editor.x86_64.tracy_ai_full.exe`
- 测试项目：`C:\CodeProjects\GodotProjects\tps-demo`
- 平台：Windows x64，Visual Studio 18 / MSVC 14.51，Release，C++20
- 真实负载：D3D12 完整内存追踪、调用栈、Frame Image、锁、消息和 CPU Zone
- 正式证据根目录：
  `C:\Users\Admin\Documents\JN-Unity-T3\tracy-formal-validation-20260726`

构建使用相互独立的 `build-stream`、`build-capture-clean2`、`build-query` 和
`build-update` 目录，避免旧可执行文件误入结果。最终增量重编译覆盖采集器、
转换器、stream 工具、测试生产者、journal 测试、Query 及两组 Query 测试。

## 3. 验收标准

正式通过要求同时满足：

- Release 编译成功，相关 CTest 全部通过；
- journal 只发布带提交尾的有效前缀，CRC、序号和水位单调；
- 正常停止有且只有一个 `SessionEnd`，异常停止不伪造完整状态；
- 未结束的已提交 revision 可以并发读取，部分尾部不污染旧视图；
- 原始 `.tracy-stream` 可由 Query/MCP 直接访问；
- 严格转换必须重放客户端协议，并验证服务器响应，无缺包或内容漂移；
- stream、转换结果和符号补全结果的关键语义一致；
- 截断、损坏、强停和 Client 无响应都有明确且有界的行为；
- 历史 v1、v2、v3 与当前 v4 journal 都能读取和转换；
- 参数错误必须显式失败，不能悄悄关闭安全机制。

## 4. Release 构建和自动化测试

| 测试集 | 结果 | 覆盖重点 |
| --- | ---: | --- |
| `build-stream` CTest | 2/2 通过 | journal、恢复、Store、背压、协议观察、版本 |
| `build-query` CTest | 4/4 通过 | Query 合同、MCP transcript、版本、doctor |
| Replay transcript 单元用例 | 8 类通过 | 相同、合法重排、改字节、参数包、跨客户端/控制边界、分片重排、缺包 |
| 最终合成端到端 | 通过 | 正常录制、实时 revision、排空、强停、转换、MCP |

最终合成验收产物为
`acceptance-final-incremental\stream-acceptance-20260726-071533-138`：

- 主 journal：20,252 字节、158 条记录；
- 录制中观察到 31 个不同 revision，且水位不回退；
- ProtocolOnly 主动排空：164 条记录，排空标记后仅 6 字节客户端 payload；
- 强停样本：19 条有效记录，无伪造 `SessionEnd`；
- 三次严格转换均成功；
- 12 个 MCP 工具全部实际调用成功。

编译只有两个既存的代码页 C4819 警告，来源为
`tracy_robin_hood.h` 和第三方 `ppqsort/parameters.h`，没有编译或链接错误。

## 5. 格式、恢复与命令行

### 5.1 历史格式兼容

最新转换器和 Query 对四代真实 journal 重新执行了直接 doctor、严格转换和
转换后 doctor，结果全部通过：

| 格式 | 输入字节 | 转换后字节 | 直接 Query | 严格转换 | 转换后 Query |
| --- | ---: | ---: | ---: | ---: | ---: |
| v1 | 17,956 | 4,069 | 通过 | 通过 | 通过 |
| v2 | 19,568 | 4,189 | 通过 | 通过 | 通过 |
| v3 | 20,083 | 4,170 | 通过 | 通过 | 通过 |
| v4 | 21,096 | 4,313 | 通过 | 通过 | 通过 |

新录制统一写 v4；兼容读取没有回写或偷偷升级旧文件。

### 5.2 截断、损坏和强停

单元测试覆盖无效文件头、记录 CRC、提交尾、末尾垃圾、显式截断恢复、
`SessionEnd` 后增长拒绝以及恢复后继续追加。真实 TPS 强停样本为
80,327,538 字节，`valid_size` 等于文件大小，19,494 条记录，无
`SessionEnd`，但 `recoverable_prefix=true`；其有效前缀成功转换为
24,536,025 字节 `.tracy` 并通过 doctor。

### 5.3 CLI 负向测试

最终采集器通过 11 个边界用例：缺少输出、`-d -1`、`-d 3601`、字母、
尾随字符、整数溢出、合法边界 0 和 3600、非法录制时长、非法端口、两种
输出使用相同路径。错误码和诊断均符合预期，且没有创建测试输出文件。

审计过程中发现 `atoi("abc") == 0` 会把无效 `-d` 悄悄解释为“关闭看门狗”。
现已改为完整十进制解析；非法、部分合法或溢出文本均以退出码 4 拒绝。

## 6. 真实 Godot/TPS 矩阵

最终矩阵启用 `TRACY_SYMBOL_OFFLINE_RESOLVE=1`，共 6/6 通过：

| 用例 | 墙钟时间 | journal 字节 | 记录数 | 转换后字节 | 结果 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Dual 5 秒 | 5.976 秒 | 65,926,117 | 25,588 | 20,516,621 | 通过 |
| ProtocolOnly 5 秒 #1 | 5.613 秒 | 73,767,774 | 18,554 | 22,748,622 | 通过 |
| ProtocolOnly 5 秒 #2 | 5.588 秒 | 71,589,655 | 18,413 | 22,243,094 | 通过 |
| ProtocolOnly 5 秒 #3 | 5.642 秒 | 68,359,037 | 18,330 | 21,515,861 | 通过 |
| ProtocolOnly 60 秒 | 60.578 秒 | 98,726,781 | 25,167 | 30,508,968 | 通过 |
| 真实进程强停 | 10.617 秒后终止 | 80,327,538 | 19,494 | 24,536,025 | 按预期恢复 |

三个短录都写入 `SessionBegin` v2、`BeginDrain` v4，使用查询窗口 5,037，
严格转换和 stream/trace 概览语义相等。60 秒用例在 10、30、50 秒分别启动
一个独立 Query doctor；三个读取都发生在 `complete=false` 的 revision，
stdout 有完整 doctor 结果、stderr 为空，最终录制仍正常结束。

ProtocolOnly 用例的采集器峰值私有内存约 73–74 MiB、工作集约 18 MiB；
Dual 用例因同时保留 Full Worker，采集器峰值私有内存约 323 MiB。这证明
当前测试负载下 ProtocolOnly 的采集端内存有界，但不是游戏主线程性能
零扰动的证明。

## 7. Query、MCP 和离线符号闭环

真实 TPS 的 `.tracy-stream` 与严格转换 `.tracy` 之间执行了完整 MCP A/B：

- MCP catalog：12 个工具、3 个资源模板；
- 会话异步加载、进度通知和取消；
- 搜索、绘图、时间线和验证任务；
- frames、zones、source 三类 compare；
- 源码资源和 PNG Frame Image 资源；
- 所有工具均被实际调用，最终 `RESULT=PASS`。

随后对最新转换结果执行 `tracy-update -r`：

- 发现 86,576 个调用栈帧；
- 分为 7 个映像组；
- 总用时 3.835 秒；
- `resolved.tracy` 通过 doctor；
- resolved trace 与原 `.tracy-stream` 再次通过同一套 12 工具 MCP A/B。

这验证了“录制时保存原始模块/地址信息，游戏结束后独立进程解析 PDB”的
完整闭环。它也避免让游戏内 Symbol Worker 承担长时间在线符号响应。

## 8. 本轮发现并修复的问题

### 8.1 排空无进展可能永久等待

在线符号压力下，Client 可能不再返回最后的定义。原逻辑只等
`Worker::IsConnected()`，没有总进展期限。现加入仅作用于 ProtocolOnly 的
`-d` 看门狗，默认 30 秒；任何已提交字节、双向协议字节、事件数或定义数
变化都会重新计时。超时使用退出码 124，不写 `SessionEnd`，保留有效前缀。

确定性故障注入在 Tracy 握手后挂起 Godot，使用 3 秒阈值：采集器从挂起
到退出 5.284 秒，退出码 124；8,888,459 字节全部有效，4,651 条记录，
`complete=false`、`recoverable_prefix=true`。原 stream doctor、严格转换和
转换结果 doctor 全部通过。

### 8.2 独立定义查询顺序被误判为协议损坏

ProtocolOnly 实时处理和 Full 离线重放由不同线程调度定义查询。某份真实
样本在连续查询批次中交换了 `ThreadString` 与 `CallstackFrame` 的顺序，
payload 集合完全相同，旧校验却在第 22 条记录报发散。

共享 `VerifyServerTranscript` 现在：

- 对握手、Terminate、Disconnect、DataTransfer 和 DataTransferPart 保持
  原始顺序及完整字节一致；
- 只在 journal 序号连续、全部属于独立定义查询的批次内，比较完整 payload
  的排序多重集；
- 不跨客户端记录或控制记录形成批次；
- 包数量、类型、地址、附加字段或任一字节变化仍失败。

转换器和 Query 增量读取服务器记录，只缓存当前连续定义批次，并用固定
13 字节数组排序比较；控制包即时校验，批次结束立即清空。因此额外内存与
“最大连续定义批次”相关，不再随整场录制的服务器记录总数线性增长。

曾经必现失败的 68,148,542 字节真实 journal 现已成功验证 3,254 条客户端
记录和 20,222 条服务器记录，转换为 21,080,588 字节 `.tracy`；直接 doctor
连续运行 10 次全部通过。对应负向单元测试也全部通过。

### 8.3 正式脚本的大型 JSON 误报

PowerShell 直接把多行原生输出管给 `ConvertFrom-Json` 时，较大的
`inspect --records` 结果会在约 16 KiB 后被当成不完整对象。产品单独输出的
18,288 字符 JSON 可完整解析，142 条记录与声明数量一致。验收脚本现先将
所有输出行显式连接，再解析 JSON，并保存原生命令退出码；修复后最终端到端
流程完整通过。

## 9. 残余风险和不纳入本功能的问题

### 9.1 在线符号模式只保证有界，不保证完整结束

在线 DbgHelp 模式的三次 5 秒高细节短录中，一次完整结束，两次在 30 秒
无进展后以 124 结束并保留可恢复前缀；同模式的 60 秒录制可以完整通过。
结果说明它具有调度依赖。默认看门狗解决了无限增长和无人值守卡死，但高
细节自动化仍应使用官方离线符号模式。

### 9.2 传统 Full `.tracy` 快照存在既有间歇问题

单独对传统 `-o` Full 快照做三次可靠性探针，当前起点版本为 2/3 通过；
历史 `c2dc6c47` 同样为 2/3，失败均为“文件在声明记录读完前结束”。这说明
该现象至少早于本轮 stream 修改，不是 `.tracy-stream` journal 或严格转换
引入的问题。本轮最终 Dual 用例通过，但传统快照路径仍需另立任务定位，
不能用它否定或替代 stream 的验收结果。

### 9.3 尚未覆盖的生产条件

- 数小时或全天连续录制；
- 真实慢盘、磁盘写满、突然断电；
- 关闭/开启 Tracy 的游戏主线程 p50、p99、p999 对照；
- Client 串行队列扩容点和硬内存上限；
- GUI 原生打开仍在增长的 `.tracy-stream`；
- 录制结束后自动转换和自动符号补全。

这些项目不影响当前功能正确性结论，但在进入长期生产采集前应按风险备忘录
的触发条件追加专项测试。

## 10. 退出码和运维解释

| 退出码 | 含义 | 文件处理 |
| ---: | --- | --- |
| 0 | 正常完成 | 有完整 `SessionEnd`，可直接查询和转换 |
| 4 | 命令行或输出配置错误 | 修正参数后重试 |
| 124 | ProtocolOnly 排空连续无进展 | 不完整但已提交前缀可查询、恢复和转换 |
| 130 | 排空期间第二次 `Ctrl+C` 强停 | 不伪造完整状态，保留已提交前缀 |

`124` 和 `130` 不是完整录制成功，自动化必须明确区分；但两者也不等于
已提交数据损坏。

## 11. 证据位置

- 最终真实矩阵：`real-tps-offline-symbol-after-replay-fix\summary.json`
- 最终合成验收：
  `acceptance-final-incremental\stream-acceptance-20260726-071533-138`
- 看门狗故障注入：`drain-idle-watchdog\20260726-062508-483\result.json`
- 历史格式：`compatibility-latest`
- 在线符号对照：`real-tps-after-watchdog-v2\summary.json`
- 离线符号三次探针：`real-tps-offline-symbol-probe\summary.json`
- 传统快照探针：`snapshot-reliability-current-2717b2a4\summary.json`

以上路径均位于第 2 节所列正式证据根目录。
