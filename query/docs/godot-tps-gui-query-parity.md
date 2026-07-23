# Godot TPS 管理员录制与 GUI—Query 全量对照验收

本文档用于验证同一个已保存 `.tracy` 文件在以下两条读取路径中的语义一致性：

- 人工路径：`tracy-profiler.exe` GUI；
- AI 路径：`tracy-query.exe` 的 JSON、NDJSON 与 MCP STDIO。

本文档不是性能结论报告。带 `PhaseScale < 1.0` 的 smoke trace 只用于功能覆盖、协议验证和 GUI—Query 对照，不得作为 baseline/stress 性能比较样本。

## 1. “全量”验收的准确含义

“Query 能获得全部信息”由四层证据共同证明，不能只靠在 GUI 中随机点几个面板：

1. **字段穷举层**
   - `coverage-fields-v1.json` 对照 Tracy 0.13.1 的保存、加载、Worker getter 和 GUI consumer，逐项记录持久化字段。
   - 每个必需字段必须有 DTO、Query 路径和确定性测试。
   - GUI 状态、原生指针、锁对象、临时缓存等非 trace 数据明确排除并记录原因。

2. **协议穷举层**
   - `QueryMethodRegistry` 当前有 98 个 public method。
   - `coverage-mcp-v1.json` 和 MCP transcript test 证明每个 public method 都可经 MCP 到达。
   - 12 个模型友好工具只是工作流入口，不是数据能力的删减；`tracy_inspect(method, params)` 可到达全部只读方法。

3. **真实 trace 全域层**
   - 对真实 TPS trace 调用所有 95 个单 trace 方法，再调用 3 个 compare 方法。
   - 存在的数据必须成功；确实不存在的数据必须返回明确的 `CAPABILITY_UNAVAILABLE`。
   - 所有域执行 count、分页、ref 回查、关联和 bounded resource 验证。

4. **GUI 语义对照层**
   - 对 GUI 可展示的每一种统计和关系逐项对照。
   - 对事件量巨大的域采用“总数全量 + 分层抽样”的人工方法：首项、中间项、末项、最大值、边界项、未完成项各取样。
   - GUI 不展示的 raw code、raw bytes、版本可用性、opaque ref、cursor 等字段由前 3 层证明，不能伪称为 GUI 人工可见。

只有四层均通过，才可声明 saved-trace Query 的字段、协议和 GUI 语义完整。Producer 是否实际产生某个可选域是另一项独立结论。

## 2. 当前固定验收基线

### 2.1 源码提交

| 仓库 | 分支 | 功能提交 |
| --- | --- | --- |
| Godot | `feature-tracy` | `2d3e26b53196a505ea066c14d5e98134bcf586f3` |
| TPS demo | `feature-tracy-demo` | `cddb00ba98c95bf15dea277066a617bdca7b7506` |
| Tracy | `feature-StructuredData` | `e570b399fe9999588292e5464205d8782d72715d`（Source Compare 截断语义修复；之后可附带纯文档提交） |

任何一项源码、编译选项、PDB、GPU 驱动或 trace 文件变化，都必须新建验收记录，不能沿用本文的具体数值。

### 2.2 当前二进制

| 文件 | 字节 | SHA-256 |
| --- | ---: | --- |
| `godot.windows.editor.x86_64.tracy_ai_full.exe` | 174,082,560 | `BBC317AAA3577C0E7549DF1A5F87E5F5C21FAD3164CC50E40437503EFAA23C3E` |
| `tracy-capture.exe` | 10,681,856 | `EC83014B5B874A91F0D4A9FFE423CE27BA611E3B0CF46291D4C244D26C39C8A5` |
| `tracy-query.exe` | 12,117,504 | `40F4173BB5C65E39D728C9A133B2CE05DBA66B5EAC025717C2B88D5522517E76` |
| 自定义 `tracy-profiler.exe` | 26,559,488 | `99AF547C207B58D68AD30C4412F1F2E9955DB6FC730E96AF12F07AD936C6F9F4` |
| 原始发行版 `windows-0.13.1\tracy-profiler.exe` | 26,412,032 | `1C7D6321E602B6A0B94A0897B70183AB62D4D56543C10ACD9BFC1B0424B48A78` |

正式 GUI 对照必须使用自定义 Profiler：

```text
C:\CodeProjects\GodotProjects\tracy-profiler-build\Release\tracy-profiler.exe
```

原始发行版只用于“不被覆盖”的回归检查，它不包含本项目新增的共享 Memory/GTMEM1 GUI。

### 2.3 当前 smoke 功能 trace

```text
C:\CodeProjects\GodotProjects\tracy-captures\TPS-gated-smoke-001.tracy
```

| 项 | 值 |
| --- | --- |
| 文件字节 | 44,640,223 |
| 文件 SHA-256 / Query fingerprint | `523f7fc1f05f5d38d39e7e32243b40ecc69be8634f4412ab8352842c47136410` |
| 模式 | `coverage`、`workload`、`PhaseScale=0.05` |
| Seed | `424242` |
| 窗口 | 960×540 |
| 最大 FPS | 60 |
| Renderer | D3D12 |

该 trace 已通过 Query doctor、95 个单 trace 方法、3 个 compare 方法、MCP STDIO 和下文列出的证据检查。

## 3. 管理员录制操作

### 3.1 录制前环境冻结

录制前完成并记录：

- 使用交流电源，Windows 电源模式固定；
- 固定 GPU 驱动、显示器拓扑、HDR、分辨率和刷新率；
- 关闭编辑器、浏览器、overlay、录屏、构建、杀毒扫描和无关 GPU 任务；
- 保持同一 Godot、Tracy、TPS commit 和 PDB；
- 等待前一次 shader/import 工作完成；
- 确保目标文件不存在；
- 确保没有其他 Tracy GUI、capture 或 Query 占用所选端口；
- baseline 与 stress 使用相同 seed、窗口、FPS、PhaseScale 和端口；
- 先做一次不计入统计的 coverage warm-up。

不要在 capture 保存完成前强杀 Godot 和 `tracy-capture.exe`。同时终止两者会生成截断或损坏文件。

### 3.2 确认管理员身份

右键 Windows PowerShell，选择“以管理员身份运行”，然后执行：

```powershell
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
```

必须输出 `True`。Godot 与 `tracy-capture.exe` 必须从同一个管理员 shell 启动，不能只提升其中一个进程。

### 3.3 无写入预检

```powershell
Set-Location 'C:\CodeProjects\GodotProjects\tps-demo'

.\benchmark\run_tracy_capture.ps1 `
  -Scenario coverage `
  -RunId coverage-full-001 `
  -Seed 424242 `
  -Port 8093 `
  -PhaseScale 1.0 `
  -MaxFps 120 `
  -Width 1280 `
  -Height 720 `
  -CaptureSeconds 60 `
  -TimeoutSeconds 300 `
  -OutputPath 'C:\CodeProjects\GodotProjects\tracy-captures\TPS-coverage-full-001.tracy' `
  -PlanOnly
```

确认输出中的 Godot、capture、Query、输出路径、端口、窗口和时间均正确。

### 3.4 录制 coverage

去掉 `-PlanOnly`，其余参数保持不变：

```powershell
.\benchmark\run_tracy_capture.ps1 `
  -Scenario coverage `
  -RunId coverage-full-001 `
  -Seed 424242 `
  -Port 8093 `
  -PhaseScale 1.0 `
  -MaxFps 120 `
  -Width 1280 `
  -Height 720 `
  -CaptureSeconds 60 `
  -TimeoutSeconds 300 `
  -OutputPath 'C:\CodeProjects\GodotProjects\tracy-captures\TPS-coverage-full-001.tracy'
```

正常阶段顺序是：

```text
Godot preload（Tracy 未连接）
-> scene-ready gate
-> tracy-capture 启动并连接
-> capture gate release
-> 完整 workload
-> tracy-capture 主动断开并保存
-> Query doctor
-> 脚本清理 Godot 与控制文件
```

`CaptureSeconds` 是 capture 的主动录制时长，`TimeoutSeconds` 只是异常情况下的硬上限；两者不能设置成相同的“同时强杀时间”。

### 3.5 录制匹配的 baseline/stress

coverage 成功并完成一次 GUI—Query 功能检查后，再从同一个管理员 shell 顺序执行：

```powershell
.\benchmark\run_tracy_capture.ps1 `
  -Scenario baseline `
  -RunId baseline-full-001 `
  -Seed 424242 `
  -Port 8093 `
  -PhaseScale 1.0 `
  -MaxFps 120 `
  -Width 1280 `
  -Height 720 `
  -CaptureSeconds 60 `
  -TimeoutSeconds 300 `
  -OutputPath 'C:\CodeProjects\GodotProjects\tracy-captures\TPS-baseline-full-001.tracy'

.\benchmark\run_tracy_capture.ps1 `
  -Scenario stress `
  -RunId stress-full-001 `
  -Seed 424242 `
  -Port 8093 `
  -PhaseScale 1.0 `
  -MaxFps 120 `
  -Width 1280 `
  -Height 720 `
  -CaptureSeconds 60 `
  -TimeoutSeconds 300 `
  -OutputPath 'C:\CodeProjects\GodotProjects\tracy-captures\TPS-stress-full-001.tracy'
```

正式性能结论至少录制 3 组，并交替顺序：

```text
baseline-001 -> stress-001
stress-002 -> baseline-002
baseline-003 -> stress-003
```

比较中位数、p95、p99 和完整分布，不用一次运行的最大值下结论。

### 3.6 每个文件的录制完成门槛

只有同时满足以下条件才接受：

- 脚本退出码为 0；
- `tracy-capture` 自己正常退出并写完文件；
- `.ready` 和 `.start` 临时文件已清理；
- `tracy-query --doctor` 返回 `trace_loader=ok`；
- `capture_window_begin` 恰好 1 条；
- `scenario_complete` 恰好 1 条；
- `capture_exit` 恰好 1 条；
- `scenario_failed` 为 0 条；
- `scenario_complete` 中 `resource_errors=0`；
- `scenario_complete` 中 `pending_resources=0`；
- resource、shader、physics、memory 计数均大于 0；
- CPU zone、GPU zone、memory、lock、context switch、sample、source cache 和 frame image 均有数据；
- 保存 SHA-256，并在后续 GUI 和 Query 验收中保持不变。

## 4. 自动化预检

### 4.1 Query doctor

```powershell
$query = 'C:\CodeProjects\GodotProjects\tracy-query-completeness-build\Release\tracy-query.exe'
$trace = 'C:\CodeProjects\GodotProjects\tracy-captures\TPS-gated-smoke-001.tracy'

& $query `
  --doctor `
  --allow-root 'C:\CodeProjects\GodotProjects' `
  --trace $trace
```

必须满足：

- `json_schema=ok`
- `domain_coverage_manifest=ok`
- `field_coverage_manifest=ok`
- `mcp_coverage_manifest=ok`
- `trace_loader=ok`
- fingerprint 等于文件 SHA-256。

### 4.2 正式 Query 测试

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'

& $cmake --build 'C:\CodeProjects\GodotProjects\tracy-query-completeness-build' --config Release --parallel
& $ctest --test-dir 'C:\CodeProjects\GodotProjects\tracy-query-completeness-build' -C Release --output-on-failure
```

当前正式结果为 7/7 passed。

### 4.3 真实 MCP STDIO 验收

仓库中的脚本验证实际 EXE 的单行 JSON-RPC framing，而不是只调用进程内 C++ 类：

```powershell
Set-Location 'C:\CodeProjects\GodotProjects\tracy-0.13.1'

.\query\tests\RealTpsMcpAcceptance.ps1 `
  -QueryExe 'C:\CodeProjects\GodotProjects\tracy-query-completeness-build\Release\tracy-query.exe' `
  -BaselineTrace 'C:\CodeProjects\GodotProjects\tracy-captures\TPS-gated-smoke-001.tracy' `
  -CandidateTrace 'C:\CodeProjects\GodotProjects\Test-GPUMemory-Fresh1.tracy' `
  -AllowRoot 'C:\CodeProjects\GodotProjects'
```

必须以 `RESULT=PASS` 结束，并满足：

- MCP `2025-11-25` initialize；
- ping、logging、initialized notification；
- 12/12 tools；
- 3/3 resource templates；
- progress notification；
- cancellation；
- 两个 session 串行加载并 Ready；
- overview/search/inspect/timeline/analyze；
- validation async job；
- frames/zones/source 三类 compare；
- source text resource；
- PNG frame-image resource；
- 两个 trace 正常 close；
- stdout 不含非 JSON 协议文本。

当前真实运行耗时 14.6 秒，12/12 工具均执行，三类 compare 均成功。

### 4.4 全方法真实调用结果

当前 TPS trace 的 98 个 public method 结果：

| 类别 | 数量 | 结果 |
| --- | ---: | --- |
| 单 trace 方法 | 95 | 91 成功，4 个 hardware-sample 方法明确返回 `CAPABILITY_UNAVAILABLE` |
| Compare | 3 | frames、zones、source 全部成功 |
| 非预期错误 | 0 | 通过 |
| 最大响应 | 65,316 bytes | 小于 8 MiB |

这 4 个 unavailable 不是 Query 映射缺口：当前 trace 没有硬件计数器样本，capability 明确为 absent。管理员权限不能凭空生成客户端没有写入或平台没有提供的事件。

## 5. 当前 smoke trace 的固定证据

### 5.1 Trace counts

| 数据 | Count |
| --- | ---: |
| Frame sets / frames | 3 / 3,507 |
| CPU zones | 130,359 |
| GPU zones | 4,280 |
| Threads | 2,370 |
| Context switches | 93,970 |
| Samples | 345,867 |
| Kernel samples | 119,170 |
| Ghost zones | 2,975,864 |
| Locks events / lock identities | 68,355 / 2 |
| Memory events / pools | 1,588,476 / 8 |
| Messages | 94 |
| Persisted plot points / plot series | 102,552 / 61 |
| Frame images | 36 |
| Source locations | 535 |
| Embedded source files / bytes | 823 / 31,193,741 |
| Symbols / symbol code bytes | 12,476 / 5,446,975 |
| Callstack payloads / frames | 186,502 / 262,679 |

`trace.overview.counts.plots` 是持久化 plot event 计数；`plot.list` 还包含由 Memory 数据派生的可查询 series，因此不能把两者误当成同一个计数。

### 5.2 Producer marker

`capture_window_begin`：

```text
ref: tracy:v1:523f7fc1f05f5d38:message:5
time_ns: 4115225611
thread_ref: tracy:v1:523f7fc1f05f5d38:thread:1129c
callstack_ref: tracy:v1:523f7fc1f05f5d38:callstack:965
driver=d3d12
node_count=3330
robots=4
phase_scale=0.05
resolution=960x540
```

`scenario_complete`：

```text
ref: tracy:v1:523f7fc1f05f5d38:message:58
time_ns: 5758858565
enemies_destroyed=4
memory_blocks_created=18
memory_bytes_created=9437184
physics_bodies_created=18
resources_requested=1
resources_loaded=1
resource_errors=0
pending_resources=0
shaders_created=12
```

### 5.3 Frame statistics

主 frame set：

```text
ref: tracy:v1:523f7fc1f05f5d38:frame-set:0
name: Frames
count: 1082
frame_offset: 36
median: 16,666,029 ns
p95: 33,270,365.3 ns
p99: 53,016,161.92 ns
```

前两个超长 frame 是 on-demand/capture 边界：

| Query internal index | Ref | Duration |
| ---: | --- | ---: |
| 0 | `tracy:v1:523f7fc1f05f5d38:frame:0` | 1,072,449,488 ns |
| 1 | `tracy:v1:523f7fc1f05f5d38:frame:1` | 1,844,538,014 ns |

它们用于验证边界语义，不用于性能结论。稳定的人工对照 frame：

```text
Query internal index: 68
GUI displayed frame: 103
ref: tracy:v1:523f7fc1f05f5d38:frame:44
begin_ns: 5818802714
end_ns: 6115107184
duration_ns: 296304470
```

GUI 对默认 frame set 使用 `displayed = internal_index + frame_offset - 1`，所以 Query index 68 对应 GUI Frame 103。Memory Inspector 同时显示 `Internal index 68`，人工记录必须写下两者，禁止只写“Frame 68”。

### 5.4 CPU statistics

| Zone | Source ref | Count | Inclusive total ns | Self total ns | Running total ns |
| --- | --- | ---: | ---: | ---: | ---: |
| Godot Worker Idle Wait | `source:6` | 8,778 | 72,091,785,981 | 72,091,785,981 | 175,607,196 |
| Godot WASAPI Audio Iteration | `source:3` | 14,578 | 20,053,058,465 | 20,053,058,465 | 191,644,856 |
| OS_Windows::run | `source:21` | 1,078 | 20,045,237,753 | 12,588,562 | 4,840,467,008 |
| Main::iteration | `source:22` | 1,078 | 20,032,649,191 | 7,194,287 | 4,828,910,761 |
| OS::add_frame_delay | `source:20` | 1,079 | 15,993,584,716 | 15,993,584,716 | 1,101,061,987 |

等待 zone 的 inclusive 很大而 running 很小是正常含义，不得把 `Godot Worker Idle Wait` 直接报告为 CPU 执行热点。GUI 和 Query 必须分别对齐 inclusive/self/running。

### 5.5 GPU statistics

| Zone | Source ref | Count | GPU total ns |
| --- | --- | ---: | ---: |
| Render Opaque Pass (L11) (Draw) | `source:ffdc` | 56 | 15,371,801 |
| Render Depth Pre-Pass (L2) (Draw) | `source:fff0` | 62 | 11,886,581 |
| Gather Samples (L5) (Compute) | `source:ffe8` | 63 | 5,392,388 |
| Gather Samples (L4) (Compute) | `source:ffea` | 62 | 4,964,347 |
| Shadow Render (L3) (Draw) | `source:ffec` | 55 | 4,738,036 |

### 5.6 Thread 与调度

Godot main thread：

```text
ref: tracy:v1:523f7fc1f05f5d38:thread:1129c
native_id: 70300
process_id: 81800
name: Main thread
zone_count: 91476
sample_count: 47119
kernel_sample_count: 7615
context_switch_count: 3484
running_regions: 3484
running_time_ns: 5464421323
migrations: 2308
```

`context_switch.statistics` 在该线程上报告：

```text
running_total_ns: 5464421323
wakeup_p95_ns: 31850.95
```

两个独立接口的 running total 必须相等。

### 5.7 Memory pools

| Pool | Ref | Events | Capture-end active count | Capture-end active bytes | ID semantics |
| --- | --- | ---: | ---: | ---: | --- |
| Default allocator | `memory-pool:0` | 1,581,710 | 14,260 | 4,114,445 | address |
| GPU D3D12 Default Buffers | `memory-pool:1` | 6,347 | 1,440 | 39,080,448 | logical allocation ID |
| GPU D3D12 Default Textures | `memory-pool:2` | 332 | 261 | 866,411,008 | logical allocation ID |
| GPU D3D12 FrameImage Readback | `memory-pool:3` | 4 | 4 | 8,388,608 | logical allocation ID |
| GPU D3D12 GPUUpload Buffers | `memory-pool:4` | 19 | 19 | 32,899,072 | logical allocation ID |
| GPU D3D12 Readback Buffers | `memory-pool:5` | 2 | 2 | 524,288 | logical allocation ID |
| GPU D3D12 Timestamp Readback | `memory-pool:6` | 2 | 2 | 4,096 | logical allocation ID |
| GPU D3D12 Upload Buffers | `memory-pool:7` | 60 | 60 | 767,819,776 | logical allocation ID |

GPU `address` 字段在这些 pool 中是 logical allocation ID，不能按 CPU 虚拟地址解释。

### 5.8 Frame 103 / internal index 68 Memory snapshot

范围：

```text
begin_ns: 5818802714
end_ns: 6115107184
consistent: true
possible_capture_baseline: false
```

全部 pool：

| State | Bytes | Allocations |
| --- | ---: | ---: |
| Active at start | 2,722,691,135 | 27,617 |
| Allocated in frame | 1,996,083 | 21,200 |
| Freed in frame | 2,270,905 | 21,834 |
| Active at end | 2,722,416,313 | 26,983 |
| Peak | 2,722,720,198 | 27,730 |

Default allocator：

| State | Bytes | Allocations |
| --- | ---: | ---: |
| Active at start | 5,008,063 | 21,016 |
| Allocated in frame | 1,996,083 | 21,200 |
| Freed in frame | 2,270,905 | 21,834 |
| Active at end | 4,733,241 | 20,382 |
| Peak | 5,037,126 | 21,129 |

所有 D3D12 pool：

```text
start = end = peak: 2,717,683,072 bytes / 6,601 allocations
allocated_in_frame: 0
freed_in_frame: 0
```

`all_transitions` 的总数是 27,783。分页首项：

```text
ref: tracy:v1:523f7fc1f05f5d38:memory-event:176ae0
address: 0x20b70ae8aa0
size_bytes: 56
allocation_zone_ref: tracy:v1:523f7fc1f05f5d38:cpu-zone:6453
allocation_callstack_ref: tracy:v1:523f7fc1f05f5d38:callstack:136
```

### 5.9 Capture-baseline GPU snapshot

Query internal index 2 对应 GUI Frame 37：

```text
ref: tracy:v1:523f7fc1f05f5d38:frame:2
begin_ns: 2916987502
end_ns: 2940002130
possible_capture_baseline: true
```

所有 D3D12 pool：

```text
active_at_start: 0 bytes / 0
allocated_in_frame: 2,704,014,976 bytes / 6,626
freed_in_frame: 0 bytes / 0
active_at_end: 2,704,014,976 bytes / 6,626
```

GUI 必须显示 `Possible capture baseline` 警告。这些是 on-demand 连接时回放的存量 allocation，不能断言它们都在这个游戏帧中真实创建。

### 5.10 GTMEM1 Pass 边界与精确配对

总 Pass 数：

```text
pass_count: 4269
protocol_present: true
complete: true
```

Pass 1–54 都属于连接边界的 Godot frame 36，CPU relation 已保存，但对应 GPU zone 在 capture 窗口外：

```text
pass 1 ref: tracy:v1:523f7fc1f05f5d38:gpu-memory-pass:1
pass 54 ref: tracy:v1:523f7fc1f05f5d38:gpu-memory-pass:36
gpu_pairing: missing
```

这 54 项必须报告 missing，不能猜测配对。

从 Pass 55 开始为 exact：

```text
pass ref: tracy:v1:523f7fc1f05f5d38:gpu-memory-pass:37
pass_id: 55
Godot frame: 37
name: Command Graph (L0) (Copy)
operations: Copy
command_count: 18
total_use_count: 18
gpu_pairing: exact
cpu_zone_ref: tracy:v1:523f7fc1f05f5d38:cpu-zone:f0d
gpu_zone_ref: tracy:v1:523f7fc1f05f5d38:gpu-zone:1
```

CPU zone：

```text
duration_ns: 58398
start_ns: 2958583180
end_ns: 2958641578
extra_valid: true
```

GPU zone：

```text
gpu_duration_ns: 3072
gpu_start_ns: 2980203633
gpu_end_ns: 2980206705
query_id: 2
```

18 个 allocation ID：

```text
92, 6075, 6109, 6143, 6177, 6220, 6268, 6269, 6270,
6271, 6272, 6273, 6274, 6275, 6276, 6277, 6278, 6279
```

每项均为：

```text
kind: B
usage_mask: 0x0000000000000006
usage: Write, Copy
```

### 5.11 Locks 与 plots

Lock contention：

| Ref | Waits | Obtains | Total wait ns | p95 wait ns |
| --- | ---: | ---: | ---: | ---: |
| `lock:0` | 16,587 | 16,587 | 3,571,616 | 359.7 |
| `lock:1` | 6,198 | 6,198 | 296,190 | 129.0 |

`plot.list`：

```text
series: 61
empty names: 0
```

固定抽样：

| Plot | Ref | Points | Min | Max |
| --- | --- | ---: | ---: | ---: |
| Default allocator | `plot:0` | 3,149,161 | 0 | 27,049,333 |
| GPU Local Usage | `plot:1c` | 1,044 | 1,352,081,408 | 2,062,426,112 |
| D3D12MA Default Allocation Bytes | `plot:32` | 74 | 905,491,456 | 1,912,241,536 |
| Godot/ResourceLoader/Pending Tasks | `plot:3c` | 25 | 71 | 83 |

任何空 plot 名称都视为失败。

### 5.12 Callstack、source 与 frame image

Marker callstack：

```text
ref: tracy:v1:523f7fc1f05f5d38:callstack:965
```

前几个可复查 frame：

| Depth | Name | File:line |
| ---: | --- | --- |
| 0 | `tracy::Callstack` | `TracyCallstack.hpp:107` |
| 1 | `tracy::Profiler::SendCallstack` | `TracyProfiler.hpp:804` |
| 2 | `tracy::Profiler::Message` | `TracyProfiler.hpp:452` |
| 3 | `__print_line` | `godot\core\string\print_string.cpp:101` |
| 6 | `VariantUtilityFunctions::print` | `godot\core\variant\variant_utility.cpp:963` |
| 7 | `GDScriptFunction::call` | `godot\modules\gdscript\gdscript_vm.cpp:2422` |

首个 frame image：

```text
ref: tracy:v1:523f7fc1f05f5d38:frame-image:0
width: 320
height: 180
raw_bc1_bytes: 28800
frame_ref: tracy:v1:523f7fc1f05f5d38:frame:19
resource URI: tracy://trace/{runtime_trace_id}/frame-image/0
```

MCP 实测 PNG Base64 长度为 39,952 字符；解码必须是有效 320×180 PNG，方向和颜色需人工检查。

### 5.13 Validation 的预期结果

```text
valid: true
checks: 15
error_count: 0
finding_count: 3
```

| Severity | Code | Count | 解释 |
| --- | --- | ---: | --- |
| warning | `INCOMPLETE_CPU_ZONES` | 32 | 固定 capture 边界切断的长生命周期/等待 zone |
| warning | `MISSING_GTMEM_GPU_ZONE` | 54 | 连接边界 frame 36 的 Pass 1–54 |
| info | `CAPABILITY_ABSENT` | 0 | `hardware_sample` 未出现在该 snapshot |

出现新的 error、ambiguous GTMEM1 配对、`scenario_failed` 或未知 warning 时，当前 trace 不得静默通过。

## 6. GUI—Query 人工对照步骤

### 6.1 固定文件

先在普通 PowerShell 记录文件 hash：

```powershell
Get-FileHash -Algorithm SHA256 `
  'C:\CodeProjects\GodotProjects\tracy-captures\TPS-gated-smoke-001.tracy'
```

然后启动自定义 Profiler：

```powershell
& 'C:\CodeProjects\GodotProjects\tracy-profiler-build\Release\tracy-profiler.exe' `
  'C:\CodeProjects\GodotProjects\tracy-captures\TPS-gated-smoke-001.tracy'
```

不要同时打开另一个 Tracy client。读取已保存 trace 不要求管理员权限。

### 6.2 数值比较规则

- Query 的时间/字节/ID 使用无损字符串；GUI 会格式化为 ns、µs、ms、MiB 或 GiB。
- 先比较原始 count，再比较换算后的显示值。
- 时间区间统一为半开区间 `[begin_ns, end_ns)`。
- `end_ns=null` 表示未完成事件，不能当作 0。
- CPU inclusive、self 和 running 是三个不同指标。
- GPU allocation 的 `address` 是 logical allocation ID。
- GUI displayed frame 与 Query internal index 不是同一个数字。
- ref 是 trace-scoped；换 trace 后禁止复用。

### 6.3 对照矩阵

在“人工结果”列填写 `PASS`、`FAIL` 或带理由的 `N/A`，不得留空后宣告完成。

| ID | 域 | GUI 检查点 | Query/MCP 检查点 | 通过条件 | 人工结果 |
| --- | --- | --- | --- | --- | --- |
| QG-001 | Trace info | Trace information | `trace.info`, `trace.overview`, `trace.counts` | 程序、时间、CPU、PID、版本、frame offset、counts 一致 | 待测 |
| QG-002 | App info | Trace/app info 文本 | `trace.app_info` | Godot commit、D3D12、full_capture、callstack、memory、on_demand 一致 | 待测 |
| QG-003 | Crash | Crash 面板或无 crash | `trace.crash` | 两边均明确 absent | 待测 |
| QG-004 | Capabilities | 各面板有/无数据 | `system.capabilities` | present=false 不伪装为空成功 | 待测 |
| QG-005 | Threads | Timeline thread rows | `thread.list/get/statistics/timeline/migration` | main thread ID、名称、zone/sample/context count、running、migration 一致 | 待测 |
| QG-006 | CPU | CPU Data | `cpu.topology/usage/timeline` | topology、CPU index、usage 时间片一致 | 待测 |
| QG-007 | Scheduling | Context Switch 窗口 | `context_switch.*` | main thread running total 与 wake latency 抽样一致 | 待测 |
| QG-008 | Frames | Frame Overview | `frame.sets/list/get/statistics/outliers` | 3 个 set、count、median/p95/p99、index 68 一致 | 待测 |
| QG-009 | Frame mapping | GUI Frame 103 / internal 68 | `frame.range_mapping` | begin/end 和半开区间一致 | 待测 |
| QG-010 | Frame image | Frame image/playback | `frame_image.*`, MCP resource | ref、frame、320×180、方向、颜色一致 | 待测 |
| QG-011 | CPU zone | Statistics/Zone Info | `zone.cpu.*` | count、inclusive/self/running、parent/children、source/callstack 一致 | 待测 |
| QG-012 | GPU zone | GPU timeline/Zone Info | `zone.gpu.*` | context、CPU/GPU 时间、parent、query ID 一致 | 待测 |
| QG-013 | Flamegraph | Flame Graph | CPU/GPU top-down/bottom-up | 路径、count、inclusive/self 抽样一致 | 待测 |
| QG-014 | Samples | Samples/ghost zones | `sample.*` | sample/kernel/ghost count，symbol 聚合和 flame path 一致 | 待测 |
| QG-015 | Hardware samples | Hardware sampling UI | `hardware_sample.*` | 当前 trace 两边明确 absent；不得伪造 | 待测 |
| QG-016 | Callstack | Callstack 窗口 | `callstack.*` | callstack 965 的 depth、name、file、line 一致 | 待测 |
| QG-017 | Symbol/code | Symbol/assembly | `symbol.*` | 地址、名称、文件、raw code/disassembly bounded 读取一致 | 待测 |
| QG-018 | Source | Source view/cache | `source.*`, source resource | source location、嵌入文件、line range 一致 | 待测 |
| QG-019 | Memory pools | Memory plot/pool | `memory.pools/events/get` | 8 pool 名称、event、active count/bytes、ID 语义一致 | 待测 |
| QG-020 | Frame Memory | Frame memory inspector | `memory.frame_snapshot` | internal 68 的 start/alloc/free/end/peak 与 per-pool 完全一致 | 待测 |
| QG-021 | Memory diff | 两个 frame 分别检查 | `memory.diff` | index 58→68 的 added/removed/retained 和抽样 event 一致 | 待测 |
| QG-022 | Leak/callstack | Memory callstack/leak UI | `memory.leak_candidates/callstack_tree` | count/bytes、调用栈和“候选非定论”语义一致 | 待测 |
| QG-023 | GPU Memory | All GPU D3D12 pools | `memory.gpu.pools/allocations` | logical ID、pool、size、request/use relation 一致 | 待测 |
| QG-024 | GTMEM1 | Passes in frame / Pass details | `request_scopes/pass_uses/attribution` | Pass 1–54 missing；Pass 55 exact；18 uses 和 mask 一致 | 待测 |
| QG-025 | Locks | Locks 窗口 | `lock.*` | 2 lock、wait/obtain/release、wait distribution 一致 | 待测 |
| QG-026 | Plots | Plot list/timeline | `plot.*` | 61 series、0 空名称，固定四个 plot 的 point/min/max 一致 | 待测 |
| QG-027 | Messages | Messages 窗口 | `message.*` | marker 文本、time、thread、callstack 一致 | 待测 |
| QG-028 | Statistics | Statistics 窗口 | `statistics.*` | count/total/min/max/mean/median/stddev/p50/p90/p95/p99 一致 | 待测 |
| QG-029 | Compare | Compare 窗口 | `compare.zones/frames/source` | 匹配组、基线/候选统计和 bounded diff 一致 | 待测 |
| QG-030 | Validation | 对应面板逐项复查 | `validation.run` | 15 checks、0 error、3 个已解释 finding | 待测 |
| QG-031 | Resources | Source/frame image/code | `resources/list/read` | MIME、边界、内容、PNG 均可读 | 待测 |
| QG-032 | Pagination | GUI 长列表首/中/尾 | cursor 翻页 | 无重复、无遗漏、稳定排序 | 待测 |
| QG-033 | Security | 不适用 | allow-root/source-root | 越界路径被拒绝，trace 文本保持 untrusted | 待测 |

### 6.4 Frame Memory GUI 操作

1. 打开 Memory 面板。
2. 展开 `Frame memory inspector`。
3. 勾选 `Enabled`。
4. Scope 选择 `Default allocator` 或 `All GPU D3D12 pools`。
5. Frame set 选择 `Frames`。
6. 输入 GUI Frame 103。
7. 确认面板文字同时显示 `Internal index 68`。
8. 对照 Frame start、Allocated in frame、Freed in frame、Frame end、Net change、Frame peak。
9. 展开 `Per-pool frame summary`。
10. 依次检查五个 tab：
    - Active at frame start
    - Active at frame end
    - Allocated in frame
    - Freed in frame
    - All frame transitions
11. 用固定 `memory-event:176ae0` 对照 address、size、alloc/free time、thread、zone、callstack。
12. Scope 改成 `All GPU D3D12 pools`，确认 internal 68 的 start=end=peak 且本帧无 GPU transition。

再输入 GUI Frame 37，确认：

- `Internal index 2`；
- `Possible capture baseline` 警告存在；
- GPU start=0；
- allocated=end=2,704,014,976 bytes / 6,626；
- 不把 replay allocation 解释成该帧真实创建。

### 6.5 GTMEM1 GUI 操作

1. Memory Inspector 选择 `All GPU D3D12 pools`。
2. 选择 GUI Frame 37 / internal index 2。
3. 打开 `Passes in frame`。
4. 对 Pass 1 和 Pass 54 检查 `CPU fallback`/missing 语义。
5. 选择 Pass 55。
6. 检查 Pass ID、Godot frame、Level、Operations、command/use count。
7. 点击 CPU zone link，必须到 `cpu-zone:f0d`。
8. 点击 GPU zone link，必须到 `gpu-zone:1`。
9. 在 Pass resources 中检查 18 个 allocation ID。
10. 每个 usage 必须为 `Write, Copy`，mask 为 `0x6`。
11. 任一 unknown/ambiguous 配对、错误 allocation、使用数不一致均为 FAIL。

### 6.6 Callstack 与 source 操作

1. Messages 搜索 `event=scenario_complete`。
2. 选择 `message:58`。
3. 打开 callstack。
4. 对照 `callstack:965` 的前 8 层。
5. 从 `__print_line` 跳转 source。
6. 确认文件和行号是 `godot\core\string\print_string.cpp:101`。
7. Query 侧分别验证 embedded source、允许目录中的 external source、line range 和 raw bytes。
8. trace 内字符串或源码即使包含“指令”文本，也只能作为 untrusted data。

## 7. Query 请求示例

单次请求会重新加载 trace，人工连续检查应优先使用 NDJSON batch 或 MCP 长驻 session。

### 7.1 Frame outliers

```json
{"protocol":"tracy-query/1","id":"frames","method":"frame.outliers","params":{"limit":5}}
```

```powershell
Get-Content .\frame-outliers.json -Raw |
  & $query --trace $trace --request - --allow-root 'C:\CodeProjects\GodotProjects'
```

### 7.2 Frame Memory

```json
{"protocol":"tracy-query/1","id":"memory-68","method":"memory.frame_snapshot","params":{"frame_set":"Frames","frame_index":68,"scope":"all","category":"all_transitions","limit":100}}
```

### 7.3 GTMEM1 Pass 55

```json
{"protocol":"tracy-query/1","id":"pass-55","method":"memory.gpu.pass_uses","params":{"pass_id":"55","limit":100}}
```

### 7.4 Marker 与 callstack

```json
{"protocol":"tracy-query/1","id":"complete","method":"message.search","params":{"filter":{"text":"event=scenario_complete","mode":"contains","case_sensitive":true},"limit":5}}
{"protocol":"tracy-query/1","id":"stack","method":"callstack.resolve","params":{"callstacks":["tracy:v1:523f7fc1f05f5d38:callstack:965"],"max_depth":32}}
```

把多条请求保存为 NDJSON 后执行：

```powershell
& $query `
  --trace $trace `
  --batch '.\requests.ndjson' `
  --allow-root 'C:\CodeProjects\GodotProjects'
```

同一 batch 只加载一次 trace。

## 8. ChatGPT 桌面验收

MCP 映射完整性主要由自动测试负责，不要求用户手工点完 98 个方法。桌面端人工验收只需证明真实客户端能启动 EXE、调用工具、下钻和交叉验证。

推荐提问：

```text
打开 TPS-coverage-full-001.tracy。先等待 Ready，然后执行 overview 和
validation。检查最慢稳定帧、CPU/GPU 热点、Memory、GTMEM1、locks、
context switches、samples 和 producer markers。每个结论至少做一次独立
交叉验证，并引用 trace fingerprint、frame、zone、allocation、Pass、
callstack 和 source ref。不要把 capture 边界 frame 或等待 zone 的
inclusive time 直接解释成执行热点。
```

双 trace：

```text
比较 TPS-baseline-full-001.tracy 与 TPS-stress-full-001.tracy 的 Frames、
CPU/GPU Zones 和 embedded Source。先确认 seed、分辨率、PhaseScale、
Godot/Tracy/TPS commit 一致。报告 mean、median、p95、p99 和分布变化，
并用至少一个 frame ref、zone/source ref 和 producer marker 复查每个结论。
```

通过条件：

- 不打开或控制 GUI；
- 不要求 CSV、SQLite 或全量 JSON；
- 不把 `.tracy` 整体上传；
- 工具只返回 bounded JSON/source/image；
- 最终报告引用有效 ref；
- 任取 3 个结论，可用 Query 或 GUI 复查。

## 9. 不一致处理

遇到差异时立即冻结现场，记录：

```text
Trace path:
Trace SHA-256:
Profiler SHA-256:
Query SHA-256:
Godot/Tracy/TPS commits:
Domain/method:
Request JSON:
Entity ref:
GUI panel:
GUI displayed value:
Query raw value:
Unit conversion:
Screenshot:
Expected:
Actual:
Boundary/incomplete/version status:
```

按以下顺序定位：

1. 是否打开了同一个 SHA-256 文件；
2. 是否混淆 GUI frame number 与 Query internal index；
3. 是否混淆 ns/ms、bytes/MiB/GiB；
4. 是否混淆 inclusive/self/running；
5. 是否处于 on-demand capture baseline；
6. 是否为半开区间边界；
7. 是否为 incomplete event；
8. capability 是否 absent；
9. cursor 是否来自旧 session/revision；
10. GUI 与 Query 是否走同一个共享分析函数；
11. 才判断为字段映射或算法缺陷。

发现差异后不要修改 trace、不要手工“修正”输出，也不要用空数组代替 unavailable。用最小 ref 建立回归测试，修复后重新构建 Query 和 Profiler，并对同一个 SHA-256 trace 重跑。

## 10. 最终签字模板

```text
录制人:
验收人:
日期:

Trace:
SHA-256:
Scenario / Run ID / Seed:
Resolution / Max FPS / PhaseScale:
GPU driver / Windows build / Power mode:
Godot commit:
TPS commit:
Tracy commit:
Godot EXE SHA-256:
Profiler EXE SHA-256:
Query EXE SHA-256:

Query CTest: PASS / FAIL
98 public methods: PASS / FAIL
MCP STDIO: PASS / FAIL
Producer markers: PASS / FAIL
QG-001..QG-033: PASS / FAIL / explained N/A
Unexpected validation errors: 0 / ...
Random evidence refs reviewed:
1.
2.
3.

可用于功能完整性验收: YES / NO
可用于性能 baseline/stress 比较: YES / NO
遗留问题:
```

## 11. 2026-07-23 管理员全量自动验收记录

本节记录已经实际完成的录制和自动验收，不是待执行模板。三次录制均通过 UAC 提升后的 PowerShell 启动 Godot 和原生 `tracy-capture.exe`，并使用同一 Godot/TPS 源码、同一 seed、同一窗口、同一 FPS 上限和未缩放的 `PhaseScale=1.0`。

### 11.1 Trace 身份与场景

| 场景 | 文件 | 字节 | SHA-256 / Query fingerprint |
| --- | --- | ---: | --- |
| coverage | `TPS-coverage-full-001.tracy` | 286,236,330 | `015c0ffbd897425cb20e402d70ee1a3f592a1e07eb0a452b0880e6dfa00ee3a5` |
| baseline | `TPS-baseline-full-001.tracy` | 295,433,125 | `b60242033fda62619eec452e6e70e7a1034268457b63cbccdae3a5c519b8409f` |
| stress | `TPS-stress-full-001.tracy` | 277,590,226 | `a495bdd6ba62c09bd421356397d474b9a35898dae05db4dde0fdb5b0044f7391` |

固定条件：

| 字段 | coverage | baseline | stress |
| --- | ---: | ---: | ---: |
| Scenario intensity | 2 | 1 | 3 |
| Seed | 424242 | 424242 | 424242 |
| PhaseScale | 1.0 | 1.0 | 1.0 |
| 分辨率 | 1280×720 | 1280×720 | 1280×720 |
| Max FPS | 120 | 120 | 120 |
| `capture_window` marker | 1 | 1 | 1 |
| `scenario_complete` marker | 1 | 1 | 1 |
| `capture_exit` marker | 1 | 1 | 1 |
| `scenario_failed` marker | 0 | 0 | 0 |

Producer 完成证据：

| 工作负载 | coverage | baseline | stress |
| --- | ---: | ---: | ---: |
| Memory blocks | 24 | 8 | 48 |
| Memory bytes | 12,582,912 | 2,097,152 | 50,331,648 |
| Physics operations | 20 | 8 | 36 |
| Resource completed / requested | 9 / 9 | 5 / 5 | 14 / 14 |
| Resource errors / pending | 0 / 0 | 0 / 0 | 0 / 0 |
| Shader operations | 16 | 8 | 24 |
| Enemy groups | 4 | 4 | 4 |

三个文件分别运行 `--doctor --trace`，JSON Schema、domain coverage、field coverage、MCP coverage、statistics 和 trace loader 均为 `ok`。

### 11.2 全域数据计数

以下计数来自同一个 `tracy-query.exe` 对三个已保存 trace 的结构化读取。

| 数据域 | coverage | baseline | stress |
| --- | ---: | ---: | ---: |
| CPU zones | 887,204 | 893,694 | 883,606 |
| GPU zones | 73,707 | 74,692 | 72,794 |
| Frames（全部 frame sets） | 11,246 | 12,126 | 10,909 |
| Frame sets | 3 | 3 | 3 |
| Context switches | 733,738 | 794,865 | 760,143 |
| User samples | 2,004,590 | 2,185,774 | 1,927,314 |
| Ghost zones | 21,132,915 | 22,911,848 | 18,785,736 |
| Kernel samples | 851,526 | 922,827 | 898,826 |
| Locks / lock events | 2 / 176,928 | 2 / 183,387 | 2 / 176,787 |
| Memory pools / events | 8 / 20,003,967 | 8 / 20,031,475 | 8 / 20,187,986 |
| Frame images | 136 | 136 | 134 |
| Messages | 177 | 139 | 238 |
| Plot series / points | 61 / 846,146 | 61 / 872,495 | 61 / 842,211 |
| Source cache files / bytes | 972 / 35,404,652 | 1,125 / 38,650,889 | 829 / 34,885,475 |
| Symbols | 53,596 | 73,732 | 20,978 |
| Threads | 3,402 | 3,991 | 3,569 |
| Hardware samples | 0 | 0 | 0 |

`system.capabilities` 报告 23 个数据域。除 `hardware_sample` 外，实际存在的域均为 `present=true`、`queryable=true`；四个 hardware-sample 查询明确返回 `CAPABILITY_UNAVAILABLE`，没有用空数组伪装成功。管理员权限不能凭空生成 CPU PMU/hardware sample；是否采集该域必须由平台、Tracy 采样后端和运行条件共同支持。

### 11.3 Frame 分布

这里使用名为 `Frames` 的固定 frame set，不使用容易被窗口切换或 VSync 状态改变污染的辅助 frame set。

| 统计 | coverage | baseline | stress | stress − baseline |
| --- | ---: | ---: | ---: | ---: |
| Count | 4,085 | 4,089 | 4,037 | -52 |
| Median ns | 8,333,177 | 8,333,192 | 8,333,226 | +34 |
| P95 ns | 32,840,722.8 | 31,935,855.4 | 31,980,374.4 | +44,519.0 |
| P99 ns | 42,131,371.28 | 39,630,901.12 | 43,215,985.64 | +3,585,084.52 |
| Mean ns | 15,770,517.87 | 15,239,220.89 | 15,873,660.47 | +634,439.58 |
| Max ns | 3,047,688,855 | 1,773,899,951 | 2,762,556,537 | +988,656,586 |

这一轮的直接观察：

- Median 基本不变；
- P95 增加约 0.14%；
- P99 增加约 9.05%；
- Mean 增加约 4.16%，但 mean/max 会明显受 capture 启停、窗口/VSync 切换和单次长尾影响；
- 当前只有每种强度一个 run，因此这些数值用于证明 compare 数据链完整，不能单独作为最终性能回归结论。正式性能结论至少需要固定环境、多次重复录制、剔除预热/边界帧，并报告分布与置信区间。

### 11.4 Validation

三个 trace 均返回：

- `valid=true`；
- 15 个 validation checks；
- 0 个 error；
- 3 类 finding。

| Finding | coverage | baseline | stress | 解释 |
| --- | ---: | ---: | ---: | --- |
| Incomplete CPU zones | 32 | 32 | 32 | capture 边界仍在运行的 zone，保留为 incomplete，不猜测结束时间 |
| GTMEM1 missing GPU-zone boundary | 62 | 151 | 63 | 已保存 Pass 中缺少可精确配对的 GPU 边界，按歧义/缺失状态返回 |
| Hardware sample unavailable | 1 类 | 1 类 | 1 类 | trace 未包含 PMU hardware sample，capability 明确 unavailable |

coverage trace 还完成了 72,594 个 GTMEM1 Pass 的归因扫描。未完成或缺失 GPU 配对的 Pass 没有被伪造为 exact。

### 11.5 真实 MCP 双 Trace Compare

修复后的 `tracy-query.exe --mcp` 同时打开 baseline 与 stress；加载严格串行，两份 trace 都经历 `loading → indexing → ready`。从进程启动、加载两份大 trace、执行四个异步 job 到关闭 session，总墙钟时间为 28.2 秒。

| Compare | 匹配/结果 | 计算时间 |
| --- | ---: | ---: |
| Frames | 3 个 frame sets | 0.005 s |
| CPU zones | 851 个匹配键，返回变化量最大的 5 项 | 17.206 s |
| GPU zones | 270 个匹配键，返回变化量最大的 5 项 | 0.557 s |
| Source | confirmed changed 0；inconclusive 5；baseline-only 5；candidate-only 5 | 0.116 s |

Source Compare 在这轮验收中发现并修复了一个语义问题：

- 旧实现会把“两侧前 64 KiB 相同、但两侧都被响应预算截断”的大源码文件误报为 `changed`；
- 提交 `e570b399f` 后，确定内容或字节数不同才进入 `changed`；
- 相同 bounded prefix 但未读取完整尾部的文件进入 `inconclusive`，reason 为 `bounded_prefix_equal`；
- 本轮 5 个 inconclusive 文件各自已比较 65,536 字节，文件总长度为 70,595–160,078 字节；
- 同一 Godot 构建最终得到 `changed=0`，避免 ChatGPT 把预算截断误判为源码变化。

CPU Zone Top-N 主要被单次资源加载事件主导，例如 `menu.tscn`、`door.gd`。这是按绝对 mean/P95 delta 排序的正确结果，但不等于稳定态热点结论；人工复核时必须同时看 count、frame/time range 和 steady-state filter。GPU Top-N 中也存在只出现一次的动态 label，应同样检查 `presence` 和 count。

### 11.6 最终自动测试状态

| 验收项 | 结果 |
| --- | --- |
| Release 增量构建 | PASS |
| CTest | 7 / 7 PASS，12.06 s |
| Public Query registry | 98 methods |
| 单 trace 真实矩阵 | 95 次；91 success + 4 个预期 `CAPABILITY_UNAVAILABLE`；0 unexpected |
| 单 trace 最大普通响应 | 65,316 bytes，低于 8 MiB 上限 |
| MCP catalog | 12 / 12 tools；3 resource templates |
| MCP progress / cancellation | PASS |
| MCP validation job | 15 checks；3 findings；PASS |
| MCP frames / zones / source compare | PASS |
| Source resource | 13,282 chars；PASS |
| Frame-image PNG resource | Base64 39,952 chars；PASS |
| 修复后真实 baseline/stress compare | PASS；指纹、job、关闭流程均验证 |

自动化阶段至此完成。下一阶段是使用本文 `QG-001..QG-033`，在自定义 `tracy-profiler.exe` 中对同一 SHA-256 trace 做 GUI—Query 全量人工校验。人工校验应先使用 `TPS-coverage-full-001.tracy` 验证域覆盖，再使用 baseline/stress 验证 Compare；任何差异都必须保存 GUI 截图、Query request/response、实体 ref 和单位换算。
