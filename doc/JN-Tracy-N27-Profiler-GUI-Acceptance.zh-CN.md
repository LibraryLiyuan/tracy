# JN Unity × Tracy N27 Profiler GUI 验收报告

## 1. 结论

N27 Profiler GUI 的 G0～G8 实施和本机自动/可视化验收已经完成，当前产物状态为：

```text
PreviewCandidate
```

本轮实现没有修改 Protocol 90、JN trace section 12、GPU Catalog Schema 1、Unity、PackageRepo、捕获 INI 或既有 Trace。开发内容仍只位于独立 worktree 和独立分支，不合并 `dev`，也不创建 accepted 标签，等待用户完成最终人工体验确认。

关键结论：

- N27 GPU Catalog、GPU Physical Memory、Pass Reference 可在独立窗口中稳定查询和浏览。
- GUI 与 `tracy-query` 已共用同一 GPU Analysis Core；本次覆盖的方法对真实 N27 Trace 输出完全一致。
- 旧 Trace 缺少 N27 Catalog 时明确显示 `N27 GPU Catalog not present`，不会误报分析失败。
- section 9～11 的归档 ResourceGraph Trace 会进入不兼容提示，不再触发 Profiler 进程崩溃。
- CPU Memory 已成为 CPU-only 窗口，大型 Allocation 表可滚动，并提供独立的主表/Inspector。
- GPU 分析模块失败、缓存损坏或预算超限不会使 Timeline、CPU、Job、FrameImage 一并失效。

## 2. 身份与范围

| 项目 | 值 |
|---|---|
| Worktree | `C:\CodeProjects\GodotProjects\tracy-0.13.1-profiler-gui` |
| 分支 | `feature/JNTracy-Profiler-GUI` |
| 基线 HEAD | `bb0616f2db695782d4e52261ee431beb5e4b7ef4` |
| 回退标签 | `archive/pre-N27-profiler-gui-20260824` |
| Preview 标识 | `JN Unity GUI Preview` |
| Analysis Schema | 1 |
| Cache Schema | 1 |
| Protocol | 90 |
| JN trace section | 12 |
| GPU Catalog Schema | 1 |

阶段提交：

| 阶段 | 提交 | 状态 |
|---|---|---|
| G0 | `2d708d1c` | Passed |
| G1 | `32f1f3af` | Passed |
| G2 | `4c58f06c` | Passed |
| G3 | `3fd213f2` | Passed |
| G4 | `d597dd1d` | Passed |
| G5 | `c4d7188c` | Passed |
| G6 | `b2f50b3b` | Passed |
| G7 | `739b4917` | Passed |
| G8 | 本报告对应提交 | Passed for Preview；等待用户人工接受 |

## 3. 实施结果

### 3.1 Analysis Core 与缓存

- 新增不可变 `GpuAnalysisSnapshot` 和共享 `JnGpuAnalysisCore`。
- Query 的 peak、by-type、churn 等路径使用共享 Core。
- 建立异步、可取消的 GPU 分析控制器。
- 缓存使用 Trace 身份、Analysis Schema、Cache Schema 和 GUI build identity 校验。
- 缓存路径位于 `%LOCALAPPDATA%\JNTracy\ProfilerCache`，不修改原 Trace。
- 后台线程使用 BelowNormal 优先级。
- 4 GiB 软门禁进入 Partial；6 GiB 硬门禁停止扩张并返回 ResourceLimit。
- 损坏、截断或身份不匹配缓存会被拒绝并重建。

### 3.2 Capture Overview

真实 N27 Trace 可视化结果：

| Domain | Records | 状态 |
|---|---:|---|
| CPU Zones | 1,629,012 | Complete |
| Unity Jobs | 133,704 | Complete |
| GPU Zones | 228,743 | Complete |
| GPU Resource Catalog | 19,479 | Complete |
| GPU Physical Memory | 10,632 | Complete |
| GPU Pass References | 3,485,977 | Complete |
| CPU Memory pools | 9 | Complete |
| Sampling | 708,634 | Complete |
| Context Switch | 0 | NotCaptured |
| FrameImage | 7 | Complete |

Overview 正确显示 Engine/Project/AppInfo、Protocol、section、捕获域质量和 unavailable reason，并支持跳转到对应窗口。

### 3.3 GPU Memory & Resources

六个标签均已实现：

1. Overview
2. Resources
3. Physical
4. Passes
5. Lifetime & Churn
6. Quality

已验证：

- Resources 和 Allocation 使用虚拟表格，不一次创建全部 ImGui 行。
- 统一 Inspector 显示身份、分类、provenance、物理/逻辑/驻留字节、生命周期、View、Range、Pass、Quality 和关系。
- Heap Map 只绘制具有精确 offset/size 的范围。
- Alias 物理核算不重复计数。
- Pass Direct/Inclusive Working Set 使用确定性资源集合，不对子 Pass 数值直接求和。
- A/B 仅允许同一 Trace、同一 FrameSet、同一 generation。
- Lifetime & Churn 仅标记候选，不把候选直接称为泄漏。
- FrameImage 复用既有帧图片入口，不实现任意 GPU Texture 内容预览。
- CSV/JSON、Evidence Summary 和 Query JSON 导出保留 quality/exactness/unavailable 语义。

### 3.4 CPU Memory

- 原 Memory 窗口更名为 `CPU Memory`。
- 删除旧 GPU Pool/GTMEM1 UI 入口，但保留 N27 仍需要的底层数据。
- 默认使用 Frame-first 语义：`FrameSet → Frame → Allocator/MemLabel → Metric → Allocation → Callstack → Lifetime`。
- `Global / Capture End` 保留为明确的非默认模式，不再把捕获结束状态伪装成某一帧。
- 每帧提供 `Active at start`、`Allocated`、`Freed`、`Active at end`、`Peak` 和 `All transitions` 六种精确事件集合。
- Pool 汇总、Allocation、内存页、Lifetime candidate、Bottom-up/Top-down Call Stack 均由相同 Frame、Pool 和 Metric 过滤集合驱动。
- Frame A/B 冻结并校验相同 FrameSet、Pool 和 Metric；维度不一致时拒绝比较。
- 修复 `NoScrollbar/NoScrollWithMouse` 和滚动子区起点过晚导致的不可滚动结构。
- 状态/预算工具栏固定；其下建立唯一主纵向滚动区，覆盖 Frame、Pool、Allocation、Lifetime 和两棵 Call Stack 树。
- 增加 `Open allocation browser`。
- Allocation Browser 使用左侧虚拟化主表和右侧 Inspector。
- 支持 pool、活动状态、搜索、排序、callstack、source、Focus lifetime 和全局 Back/Forward。
- 项目 CPU 预算默认 16 GB；tracked allocation 与进程总量概念分开。
- 数据只覆盖跟踪池时明确显示 `TrackedOnly`。
- 删除重复的 `All CPU pools / per-pool frame summary`；CPU Memory 现在只保留一张可交互的 `Allocator / MemLabel pools` 表，顶部只显示当前选中 Pool 的紧凑摘要。
- 将原 `Unity MemLabel aggregate baseline` 改名为 `Unity MemLabel counters / coverage cross-check`，明确它是 Unity `MemoryManager::GetAllocatedMemory(label)` 每帧采样的补充计数，不是 Allocation 列表、调用栈或生命周期来源。
- Frame 模式下，MemLabel counter 选择所选帧区间内的首个实际样本；Global 模式下才使用时间线中心附近的样本，二者不再混用。
- Frame 模式的交叉核对按同一个 counter sample time 计算 `Unity aggregate`、`Tracked >=256 KiB`、差值和覆盖率；差值表示 HighEvidence 过滤口径的覆盖差，不标记为泄漏。
- 每个 MemLabel 显示 sample time、capture peak 和 evidence 状态；若所选帧内不存在 counter sample，则明确显示 previous sample/comparison unavailable，不把旧样本伪装成当前帧数据。

使用 Protocol 90 / section 8 的 StartupIOMemory Trace 验证：

- CPU pools：7。
- CPU allocation events：1,161。
- active allocations：982。
- Texture pool：976 条事件、886 条 active。
- 选中 Allocation 后可显示 16 MiB 大小、生命周期、线程和调用栈。
- 左侧列表滚动时右侧 Inspector 保持稳定。
- Frame 468 / Texture / Active-at-end 精确显示 537 个 Allocation、1310.21 MiB；帧开始为 524 个 / 1248.88 MiB，帧内新增 13 个 / 61.33 MiB。
- Frame 468 选择 Texture 后，主滚动条可从帧表和 Pool 表连续滚动至 Allocation、Lifetime 和 Call Stack，不再出现下部内容不可达。
- Frame 803 的范围为 `5:44 117,501,697 ns - 5:44 145,165,517 ns`；Texture 和 Physics 的 MemLabel counter 分别采样于约 `5:44 117,563,935 ns` 和 `5:44 117,565,412 ns`，均落在所选帧内。
- Frame 803 的 Texture 交叉核对显示 Unity aggregate `3053.49 MB`、同一时刻 tracked `3024.8 MB`、coverage `99.1%`；Physics 显示 Unity aggregate `81.24 MB`、tracked `74.01 MB`、coverage `91.1%`。

### 3.5 Job、Source Stack 与导航

- Job Inspector 显示 Schedule、Slice、Complete、Wait/Continuation 关系。
- 无 waiter 时不生成虚假返回线。
- Source UI 区分 ExactSource、PerEventExact、SiteReuse、ProfilerMarker-only 和 unavailable reason。
- 全局 Evidence History 可恢复时间范围、线程、选择、Tab 和关键滚动状态。

## 4. 自动测试与数据正确性

### 4.1 构建和自动测试

以下目标 Release 构建成功：

```text
tracy-profiler.exe
tracy-query.exe
tracy-gpu-analysis-tests.exe
tracy-query-contract-tests.exe
tracy-query-mcp-transcript-tests.exe
```

以下测试通过，退出码均为 0：

- GPU Analysis synthetic tests。
- Query contract tests。
- Query MCP transcript tests。
- `JnGpuCatalogN27QueryStaticTests.ps1`。
- soft memory budget → Partial。
- hard memory budget → ResourceLimit。
- corrupt/truncated cache → cache identity mismatch/rebuild。

### 4.2 真实 N27 Trace

Trace：

```text
C:\workflow\jnunity\build\JNTracyCaptures\N27-C8-Final-HighEvidence-Reconnect\
N27-C8-Final-HighEvidence-Reconnect.tracy
```

| 项目 | 值 |
|---|---:|
| 文件大小 | 59,923,242 bytes |
| SHA-256 | `E432E7ADC8EA053C7FDBA96D2D36C12EBD1AC85C7887137FE95163CB02BA2E39` |
| Generation | 1 |
| Catalog valid | true |
| unresolved | 0 |
| resource records | 19,479 |
| allocation records | 10,632 |
| views | 9,277 |
| logical records | 19,311 |
| parts | 1,305 |
| relations | 7,920 |
| ranges | 27,392 |
| VG records | 515 |
| Catalog record total | 101,748 |
| Catalog payload | 8,816,057 bytes |

真实显存核算：

| 项目 | 值 |
|---|---:|
| Engine Known Physical peak | 5,294,915,584 bytes（约 4.93 GiB） |
| Peak time | 394,632,657,839 ns |
| Live bytes at end | 5,291,180,032 bytes |
| Total allocated | 5,328,470,016 bytes |
| Total freed | 37,289,984 bytes |
| Create / Destroy | 7,909 / 2,723 |

旧 Query 与新共享 Core Query 对以下方法的 `data` 完全一致：

- `gpu.catalog.status`
- `gpu.memory.peak`
- `gpu.memory.by_type`
- `gpu.memory.churn`

同一进程一次打开 Trace 后连续执行 status/peak/by_type/churn/resource.search，冷执行总耗时 2,397 ms；各域内部分析耗时分别约 1/58/2/2/7 ms。

## 5. GUI 可视化验收

在 1652×992 当前桌面环境完成真实界面检查：

- Preview 标题和 build identity 正确。
- N27 Capture Overview 状态和记录数正确。
- GPU Overview 可显示 4.93 GiB physical/resident、5.33 GiB logical、6.40 GB 项目预算。
- DXGI Local Usage/Budget 未捕获时显示 `Unavailable`，没有被补零。
- Resource 表可滚动并保持 Inspector。
- 示例 Texture `ResourceId=944` 可显示名称、类型、10.7 MiB、Allocation 944、2048×1024、Mip 12、format 29、open boundary 和 exactness。
- Passes 大表可打开和滚动。
- CPU Memory Allocation Browser 可滚动、筛选和选择。
- CPU Memory 的 Frame 468 / Texture 数据、Pool 汇总和帧过滤 Call Stack 采用同一选择上下文；右侧主滚动条始终可见。
- CPU Memory 的 Pool 入口已收敛为一张 `Allocator / MemLabel pools` 表；`Unity MemLabel counters / coverage cross-check` 作为独立、可折叠的覆盖核对区存在，不再形成三个相似的 Pool 汇总入口。
- Frame 803 的 MemLabel counter 使用帧内样本并与同一时刻的 tracked allocation 事件比较，sample time、差值、coverage 和 evidence 均可见。
- Protocol 90 / section 8 Trace 在 GPU 页显示黄色 `N27 GPU Catalog not present`，CPU/Job/Timeline 保持可用。
- Protocol 88 / section 9～11 归档 Trace 显示不兼容提示，Preview 不崩溃。

## 6. 稳定性与资源门禁

| 项目 | 结果 |
|---|---|
| GPU Analysis 单元测试 | Passed |
| Query contract/MCP tests | Passed |
| Cache identity/corruption | Passed |
| 4 GiB soft limit 模拟 | Passed，进入 Partial |
| 6 GiB hard limit 模拟 | Passed，停止扩张 |
| 无 Catalog Trace | Passed，NotPresent |
| section 9～11 | Passed，安全拒绝 |
| 实际 N27 Trace 工作集 | 约 2.0 GiB |
| 实际 N27 Trace private bytes | 约 2.5 GiB |
| Profiler 进程崩溃 | 0（修复归档 Trace 初始加载异常后） |

真实 Trace 的运行内存低于 4 GiB 软上限和 6 GiB 硬上限。

## 7. 已知数据边界与未完成的人工验收

以下属于捕获数据边界或计划明确边界，不是 GUI 伪造为 0：

- 当前真实 N27 Trace 没有 DXGI Local Usage/Budget snapshot；GUI 显示 Unavailable。
- 当前真实 N27 Trace 的 Residency interval 为 0；Residency 页面不从资源存活期推断驻留。
- 当前真实 N27 Trace 没有 Periodic/Manual detailed evidence；对应状态必须为 NotSampled/NotCaptured。
- 不支持 Profiler 直接打开 `.tracy-stream`。
- 不支持持续 live GPU 深度分析；live 断开并冻结后才建立 Snapshot。
- 不预览任意 GPU Texture 内容。

本机尚未逐个完成 1080p/100%、1440p/125%/150%、4K/150%/200% 的物理显示器人工矩阵。当前分辨率已完成可视检查，代码和布局实现完成；完整多 DPI 体验仍应由用户 Preview 验收时确认。因此本报告不创建 `archive/N27-profiler-gui-accepted-*` 标签。

## 8. 最终产物

| 产物 | 路径 | SHA-256 |
|---|---|---|
| Profiler Preview | `build-profiler-gui\Release\tracy-profiler.exe` | `9288F2B977A6CB4A3C9254D5C4BD1A6BA4C27721325400AD6BA9EC18633BF155` |
| Query Preview | `build-profiler-gui-query\Release\tracy-query.exe` | `206F35A76C14B128546E472901720834DFF81A8166D2963EBF74EA98134987A6` |

当前结论：Preview 已具备用户对 N27 Trace 做正式体验验收的条件。用户明确接受后，才允许创建 accepted 标签并讨论合并到 `dev`。
