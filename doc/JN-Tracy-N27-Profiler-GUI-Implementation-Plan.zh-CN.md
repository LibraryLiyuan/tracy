# JN Unity × Tracy N27 Profiler GUI 重构实施计划

## 1. 基线与边界

- 开发分支：`feature/JNTracy-Profiler-GUI`。
- 基线：`bb0616f2db695782d4e52261ee431beb5e4b7ef4`。
- 回退标签：`archive/pre-N27-profiler-gui-20260824`。
- Preview：`build-profiler-gui/Release/tracy-profiler.exe`，窗口标识 `JN Unity GUI Preview`。
- 只修改当前 Tracy worktree；不修改 Unity、PackageRepo、Protocol 90、JN section 12、Catalog Schema 1、Query 1.32 外部契约或现有 Trace。
- N27 是首个受支持的 GPU Catalog GUI 版本；不保留 GTMEM1 GUI 兼容路径。

## 2. 架构

建立不可变 `GpuAnalysisSnapshot` 和无 ImGui 依赖的 `JnGpuAnalysisCore`。Core 负责 Resource 生命周期、Allocation/Heap/Range/Alias、Physical/Resident/Logical 核算、Pass Direct/Inclusive working set、Residency、Lifetime/Churn、VG/RTAS/Mesh 以及 quality/exactness。Profiler 和 `tracy-query` 共用该实现；Query 1.32 的方法、字段与 JSON 语义保持不变。

完整索引在首次打开 GPU 窗口时异步建立。缓存位于 `%LOCALAPPDATA%/JNTracy/ProfilerCache/<trace-sha>-<schema>-<build>`，不修改 `.tracy`。Analysis/Cache Schema 均从 1 开始。后台线程默认使用一半逻辑核心并限制在 1–8 个、BelowNormal；Fast 模式最多 `cores-2`。分析内存软上限 4 GiB、硬上限 6 GiB，达到硬上限后返回 Partial 而不是崩溃。

## 3. GUI

新增 `JN Unity Capture Overview` 与独立 `GPU Memory & Resources` 窗口。GPU 窗口包含 Overview、Resources、Physical、Passes、Lifetime & Churn、Quality 六个标签，采用固定工具栏、左右主从布局、各自单滚动区和可折叠 Evidence 区。

- 默认 FrameSet：Player.Frame → unnamed/main → Editor.Frame → Global/Lifetime。
- Overview 分离 DXGI Usage/Budget、Engine Known Physical、Resident Physical、Logical Capacity、Frame Working Set 和 Untracked。
- 默认 GiB/MiB，可切换 GB/MB；原始 bytes 永远可见。项目预算默认 GPU 6.4 GB、CPU 16 GB。
- Resources 使用虚拟化表格，Unknown 默认可见；按 Kind、Family、Allocation、Pass、Logical、Residency、Churn 分组。
- 统一 Inspector 展示 Identity、Memory、Lifetime、Usage、Views、Source、Quality、Relations，并为 Texture、Buffer、Mesh、VG、RTAS 提供专属区块。
- Physical 提供 Allocation 表、可缩放 Heap Map 和按需 Residency 时间线，只绘制精确 Range，Alias 物理字节只计一次。
- Passes 默认展示逻辑树，真实 Queue 作为 badge/segment；Direct 与 Inclusive working set 分开，Inclusive 使用确定性集合并集。
- A/B 仅比较同一 Trace、FrameSet 和 generation；FrameImage 支持缩略图、原图、前后导航和设为基线。
- Lifetime & Churn 只报告 Candidate，不宣称泄漏。
- CPU Memory 改为 CPU-only，删除旧 GPU/GTMEM1 GUI，修复无滚动结构，使用虚拟化主从表格。
- Job 增加 Schedule→Slice→Complete→Wait/Continuation 因果导航；Source Stack 明确 ExactSource、PerEventExact、SiteReuse、ProfilerMarker-only 和 unavailable 原因。

## 4. 阶段

1. G0：基线、标签、计划、测试语料和版本冻结。
2. G1：Snapshot、共享 Analysis Core、Query 适配与 golden JSON。
3. G2：后台索引、缓存、预算、取消和故障隔离。
4. G3：Capture Overview、统一导航、FrameImage 和 Preview 身份。
5. G4：GPU Overview、Resources、Physical、Inspector、Heap Map、Residency。
6. G5：Passes、Range/Subresource、A/B、Lifetime/Churn、Quality 和导出。
7. G6：CPU Memory 重构，删除旧 GPU/GTMEM1 GUI。
8. G7：Job、Source Stack 与 Evidence 导航增强。
9. G8：Preview/Query 构建、自动测试、大 Trace、DPI、截图与人工验收。

每阶段使用 `N27-GUI-G0` 至 `N27-GUI-G8` 提交。验收前不合并回 `dev`。

## 5. 验收门禁

| 指标 | 门槛 |
|---|---:|
| Capture Overview | 核心加载后 ≤200 ms |
| GPU 基础状态 | ≤500 ms |
| 首次 GPU Overview | ≤2 秒 |
| 已索引跳转 | ≤100 ms |
| 取消后台任务 | ≤2 秒 |
| 缓存命中 GPU ready | ≤5 秒 |
| UI 单次阻塞 | ≤100 ms |
| 滚动帧率 | 目标 60 FPS，最低 30 FPS |
| 分析内存 | 软 4 GiB，硬 6 GiB |

正确性必须覆盖 Texture、Mesh、Dynamic VBO、RenderGraph alias、VG、RTAS、Unknown、Bootstrap open boundary、callstack provenance、shared allocation、Pass Direct/Inclusive、NotSampled/Partial/Invalid。GUI 与 Query golden JSON 必须一致。

稳定性必须覆盖缺失 Catalog、invalid generation、gap/checksum/unresolved、损坏字符串/ID、超大数据、取消、缓存损坏、预算触顶、窗口重开、Trace 卸载和 live 断开冻结。GPU 分析失败不得影响 Timeline、CPU、Job 或 FrameImage。

## 6. 延期

不实现持续 Live GPU 深度分析、Profiler 直接读取 `.tracy-stream`、任意 GPU Texture 预览、双 Trace 并排比较、CPU retained-size、UnityObject/Asset/ResourceGraph、新 Catalog 字段、多语言和非 Windows 验收。
