# 采集配置 自动登录与录制操作

本页描述当前 mainline 的录制路径。准备一套身份明确的 Player 和工具目录后，优先使用 Capture GUI；CLI 用于可复现脚本和故障排查。

## 1 一次录制需要准备什么

| 对象 | 必须明确的内容 |
|---|---|
| Player/Editor | 实际 EXE、Client DLL/PDB、引擎与项目代码、包映射、资源版本 |
| 工具 | capture、stream-export、query、profiler 的路径和哈希；GUI 检查 Query schema 1.35.0 |
| 配置 | 启动时使用的 INI 路径/内容与环境覆盖、图形 API、Graphics Jobs、场景 |
| 输出 | 独立目录、足够空间、不会碰到旧 stop marker、录制名称和说明 |
| 对照 | 需要比较开销时，固定场景、运行路径、分辨率、画质和测量方式 |

过程存在、窗口响应、连接成功和业务场景就绪是不同状态。分析目标若要求真实游戏负载，必须等业务就绪后再开始主要窗口。

## 2 INI 选择和实际生效配置

源配置选择顺序为命令行 `-jn-tracy-config`、环境 `JN_TRACY_CONFIG`、Client DLL 相邻的默认 INI。配置在 Native 初始化前读取，编辑文件或在 GUI 改选 INI 后，需要重启目标才能改变启动配置。[JNTracyIniConfig.cpp:1](C:/workflow/jnunity/Runtime/Profiler/JNTracyIniConfig.cpp:1)

当前默认 INI 与独立 HighEvidence 不相同：默认文件启用 Frame 资源引用而关闭 Catalog；独立 HighEvidence 开启 Catalog。Marker 深度、Job 模式、Sampling、FrameImage 和 Detailed Evidence 都应逐项检查。[JNTracy.ini:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config/JNTracy.ini:1) [JNTracy.HighEvidence.ini:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config/JNTracy.HighEvidence.ini:1) [完整配置矩阵](appendices/configuration-matrix.zh-CN.md)

示例启动方式，路径替换为本次已核验的完整包：

```powershell
$playerDir = 'C:\YourPlayer\T3_development_JNTracy'
$playerExe = Join-Path $playerDir 'Game.exe'
$captureIni = Join-Path $playerDir 'JNTracy.HighEvidence.ini'
$playerLog = Join-Path $playerDir 'JNTracy-this-run.log'
Start-Process -FilePath $playerExe -WorkingDirectory $playerDir `
  -ArgumentList @('-force-d3d12', '-jn-tracy-config', $captureIni, '-logFile', $playerLog)
```

需要系统采样/ContextSwitch 时，按目标环境提供相应权限，并在 capture-context/AppInfo 验证实际数据。管理员启动本身不证明域完整。

## 3 Capture GUI 操作顺序

1. 选择匹配工具目录。当前 Work 路径要求 `tracy-capture.exe`、`tracy-stream-export.exe`、`tracy-query.exe`；查看器另行配置。
2. 启动 Player，或选择已有 Player/Editor。连接已运行的进程不应用新的 INI。
3. 选择实际文件并核对界面显示的已加载 INI。当前代码调用 QueryLoadedIni/CompareIni，明确不同时拒绝录制；显示为推断或无法确认时仍需最终回执验证。
4. 确认业务场景就绪，选择录制名称、输出目录和必要备注后开始。
5. 录制时 capture 持续写 journal，stream-export 以 follow 模式建库。混合 CPU 上工具可使用效率核；`JN_TRACY_CAPTURE_CORES=any` 是显式覆盖入口。
6. 请求停止后保持目标存活，直到 journal 排空并提示可以关闭游戏。随后 builder 完成 store；耗时分别记录在 processing_seconds。
7. 检查 complete/partial、源 SHA、store manifest/summary 和缺失域，再交分析工具。[Controller.cpp:244](../capture-gui/src/Controller.cpp#L244)

典型任务内容包括：

```text
录制目录/
  task.json
  requested.ini
  capture-status.json
  <name>.tracy-stream
  <name>.tracy-stream.store/
  store-summary.json
  attempt-<n>/...日志和证据
```

当前成功任务的 final 指向 stream，output_kind 为 store。旧 task 中的 candidate/final 字段和历史 .tracy 恢复分支仍可存在，不应据此认定新录制必须生成完整 .tracy。

## 4 CLI 录制与建库

下面是分两步执行的示例，先录制，再对已停止增长的 journal 建库。GUI 的跟随建库流程可以减少停止后的等待。

```powershell
$tracyTools = 'C:\YourTools\JNTracy\Release'
$captureDir = 'C:\YourCaptures\run-001'
New-Item -ItemType Directory -Path $captureDir | Out-Null
$streamPath = Join-Path $captureDir 'run-001.tracy-stream'
$stopPath = Join-Path $captureDir 'capture.stop'
& (Join-Path $tracyTools 'tracy-capture.exe') `
  -a 127.0.0.1 -p 8086 -j $streamPath -x $stopPath
```

在另一个终端创建同一 stop-file 请求正常停止。capture 返回后再建库：

```powershell
& (Join-Path $tracyTools 'tracy-stream-export.exe') $streamPath `
  --store-only --summary (Join-Path $captureDir 'store-summary.json')
```

不要复用已经存在的 stop marker。完整参数、资源保护和返回码以这套 EXE 的 help 为准。普通旧转换命令 `tracy-stream-convert -i ... -o ...` 仍服务于兼容路线，不是 Stream Skill 的前置步骤。

## 5 自动登录

Editor 与 Player 有独立实现。常见项目参数包括 `-jn-tracy-auto-login`、用户、服务 IP/端口、超时和稳定帧数；本机历史私服示例不能当作其他机器默认配置。[JNTracyT3AutoLogin.cs:1](C:/workflow/PackageRepo/com.jngame.tracy/Editor/JNTracyT3AutoLogin.cs:1) [JNTracyT3PlayerAutoLogin.cs:1](C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTracyT3PlayerAutoLogin.cs:1)

权威就绪应连接项目登录流程初始化、当前 loginAdapter、player/space、业务场景加载和连续稳定帧。N20/N21 的旧成功判定已被推翻，N22B 通过的是修正后的登录与稳定帧检查；那轮 HighEvidence 全域质量仍失败。[Player-T3-Auto-Login-and-Capture-Acceptance-2026-08-16.md:1](C:/Users/Admin/Documents/JN-Unity-T3/deployment-records/20260816/Player-T3-Auto-Login-and-Capture-Acceptance-2026-08-16.md:1)

自动采集脚本把这些状态与录制衔接，但每次依然要固定实际包、参数和产物身份。业务登录失败、图形设备异常或资源缺失不能靠延长固定 sleep 判为就绪。[Invoke-JNTracyPlayerAutoCapture.ps1:1](C:/workflow/PackageRepo/com.jngame.tracy/Tools/Invoke-JNTracyPlayerAutoCapture.ps1:1)

## 6 停止 恢复和质量检查

首个 Ctrl+C、定时停止或 stop-file 请求走主动断开与排空；强制停止只能形成可恢复前缀，不能伪造完整 SessionEnd。GUI 会保留失败产物和尝试编号；原始 journal 内容改变后，旧 store summary 与验证身份失效。[TracyStreamJournal.cpp:1](../stream/src/TracyStreamJournal.cpp#L1) [Controller.cpp:244](../capture-gui/src/Controller.cpp#L244)

交分析前至少核对：录制时长与预期场景、FrameSet、CPU/GPU/资源域是否存在、clean end、dropped/overflow/mismatch、有效配置、输入 SHA、store schema/revision。partial 或 Catalog 缺失可以限定分析范围，但不能解释为没有对应性能问题。
