# 单 Trace 录制、停止与转换合同

## 1. 入口与角色

支持新录制、接管活动录制、分析既有 `.tracy`、转换既有 `.tracy-stream` 和恢复中断任务。AI 负责配置核对、Capture 生命周期、转换与文件事实；用户负责确认目标场景已经准备好，或提供可验证的 ready marker。

默认目标启动模式为 `manual`。只有 `local-profile.json` 明确提供已验证的自动启动命令、日志、ready/failure pattern 时，才使用自动模式。不得用固定等待时间替代 ready 证据。

## 2. 新录制前检查

1. 运行 `resolve-profile.ps1` 并保存 resolved profile。
2. 记录任务 ID、目标、Editor/Player、场景、操作、分辨率、画质、D3D12 和 Graphics Jobs。
3. 确认请求档位为 HighEvidence；Periodic60/Manual 只能作为专项补证。
4. 确认只存在一个目标 Unity 进程和一个 JNTracy Client。
5. 确认没有 Viewer 或其他 Capture 已连接到目标 Client。
6. 确认任务目录、输出磁盘和保护阈值。
7. 确认 stop-file 在启动前不存在。
8. 不修改 Player/Editor INI；录制后以 AppInfo 的 effective config 为事实。

管理员权限由本机配置或用户处理。Skill 不创建计划任务、不操作 UAC。即使进程以管理员运行，仍须在 Trace 中验证 Sampling/Context Switch capability。

## 3. 启动 Capture

实时阶段只写 stream：

```text
tracy-capture.exe
  -a <address>
  -p <port>
  -j <task>.tracy-stream
  -d <drain-idle-seconds>
  -x <task>.stop
```

不传 `-o`，不同时生成 `.tracy`。默认不传 `-s`，由用户或 AI 明确停止。输出、错误日志和进程身份写入任务目录；不得覆盖既有文件。

启动成功必须证明：

- Capture PID 与启动时间已登记。
- 进程仍存活。
- 日志已连接或正在连接目标地址。
- stream 文件开始增长。
- 目标 Unity/Player 仍存活。

## 4. 录制中

`monitor-capture.ps1` 只监控本任务登记的 PID：

- 已录制墙钟时间。
- 连接与进程状态。
- stream 大小和最近增长。
- Capture working set。
- 输出卷剩余空间。
- 日志中的错误。

达到保护阈值时优先请求正常 stop-file 停止。保护停止不等于数据完整；最终仍看 SessionEnd、退出码和验证结果。

## 5. 正常停止

两种正式入口：

### AI/自动化

在任务目录原子创建 stop-file。Capture 检测后执行与首次 Ctrl+C相同的主动断开和协议排空。

### 人工

用户在正确的 Capture 控制台按一次 `Ctrl+C`。不要按第二次，不要关闭窗口。

共同顺序：

```text
stop requested
→ stop receiving new events
→ client disconnect
→ drain definitions/events
→ SessionEnd
→ stream close
→ Capture exit
→ file stable
```

排空完成前保持 Unity/Player 存活。第二次 Ctrl+C、关闭控制台、`Stop-Process` 或任务管理器结束会强制中止，不能标记为正常完成。

`stop-capture.ps1` 支持 `StopFile` 与 `ManualCtrlC`。Manual 模式不发送按键，只等待和验证用户已经发出的 Ctrl+C。

## 6. 完整性状态

```text
Complete
RecoverablePrefix
CorruptUnrecoverable
```

- Complete：存在完整 SessionEnd、正常退出和稳定文件。
- RecoverablePrefix：只保留已提交且可验证的前缀；允许降级分析。
- CorruptUnrecoverable：不能建立可信时间范围，停止正式分析。

RecoverablePrefix 只分析 watermark 之前的完整帧。跨越尾部的 Zone、Job、资源和 I/O 生命周期标记 `RightCensored`；不得给“没有问题”的否定性结论。

## 7. 事务式转换

```text
input.tracy-stream
→ output.tracy.converting
→ converter exit 0
→ MCP identity/doctor/domain/catalog validation
→ output.tracy
```

`convert-stream.ps1` 的 Convert 阶段只生成 candidate。MCP 完成验证后，Finalize 阶段核对 candidate SHA-256 与验证证据，再原子改名为正式 `.tracy`。

转换取消或失败：

- 原始 stream 永远保留。
- candidate 移入任务 `failed-artifacts`，不作为正式输入。
- 不在损坏 candidate 上续写。
- 不覆盖同名正式 Trace。
- Skill 记录成功/失败，不分析 converter 性能。

## 8. 既有文件

用户提供 `.tracy` 时不复制大型文件；记录绝对路径、大小、修改时间和 SHA-256。用户提供 stream 时按事务式转换执行。任何输入变化都会使旧 MCP 结果与索引缓存失效。
