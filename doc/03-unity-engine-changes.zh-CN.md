# Unity 引擎代码改动与维护说明

目标分支 feature/JNTracyUnity，HEAD `c2877d4b053c3e084b0316fc08bc6a1209e2baa8`。对比基线为 origin/T3/unity-2022.3.67f1 与该分支的共同祖先 `81d9904a2504dd2a17691b4c0cf3741e33509f37`，共 254 个文件、94,310 行新增、99 行删除；其中 84 个文件属于内嵌 Tracy 来源，82 个属于测试与验收工具。完整文件级清单见 [附录](appendices/engine-file-changes.zh-CN.md)。

## 1 单一 Client 和构建归属

Unity 主程序导入 JNTracyApi，独立 JNTracyClient 目标编译 Client 实现。RuntimeFiles 显式排除 Private 下的 Client、D3D12、Catalog、Memory、System、Usage 与 Harness，避免又编进 Unity 主程序。[JNTracyClient.jam.cs:16](C:/workflow/jnunity/Projects/Jam/JNTracyClient.jam.cs:16) [RuntimeFiles.jam.cs:245](C:/workflow/jnunity/Projects/Jam/RuntimeFiles.jam.cs:245)

```text
Unity Native / Package PInvoke / 项目插件
→ JNTracyApi C ABI
→ 唯一 JNTracyClient.dll
→ 内嵌 External/Tracy/public/TracyClient.cpp
```

全局构建开关包含 ENABLE_JN_TRACY。JN C ABI 版本为 1.8；Managed 端校验 ABI 与 ConfigHash。新 API、结构体布局或函数导出改变时，要成套检查引擎、Package 和实际部署 DLL，不能只替换声明。[GlobalDefines.jam.cs:108](C:/workflow/jnunity/Configuration/GlobalDefines.jam.cs:108) [JNTracyApi.h:22](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Public/JNTracyApi.h:22) [JNTracyRuntime.cs:124](C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTracyRuntime.cs:124)

## 2 CPU Marker 镜像与生命周期

ProfilerBridge 负责启动、producer 注册、配置读取结果、线程命名、帧边界、Marker 镜像和粗粒度直接 Zone。Profiler/ProfilerManager/Marker 中的修改把已有 Unity Profiler 标记送入桥接层，并处理同名静态 Marker 的 alias、GPU 标记能力以及 Counter 回调的激活/停用。

普通高频 Marker 可走 inline writer；其余情况保留 callback 路径。维护时必须保持其他 Profiler listener 的语义，不能因优化 Tracy 转发而丢掉其他 callback 或混淆 CPU/GPU Marker 身份。[Profiler.cpp:78](C:/workflow/jnunity/Runtime/Profiler/Profiler.cpp:78) [ProfilerManager.cpp:249](C:/workflow/jnunity/Runtime/Profiler/ProfilerManager.cpp:249) [JNTracyProfilerBridge.cpp:1816](C:/workflow/jnunity/Runtime/Profiler/JNTracyProfilerBridge.cpp:1816)

## 3 帧边界和根作用域

| 位置 | 新增内容 | 解释时的边界 |
|---|---|---|
| Application::TickTimer | EditorFrame Begin/End 与 Main.EditorTick | Editor 帧不能直接替代 Player 性能 |
| PlayerLoop | PlayerFrame Begin/End 与 Main.PlayerLoop FrameRootScope | 根区间应在本帧结束，不能被未闭合内部标记拖到后续帧 |
| Player PresentAndSync | Present Begin/End 与父帧关系 | Present 自身可能含 CPU 回收，不自动等于 GPU 等待 |
| GfxDeviceWorker | Render Frame、命令流阶段和回放上下文 | CPU Render 区间与 GPU timestamp 分开 |

[Application.cpp:2548](C:/workflow/jnunity/Editor/Src/Application/Application.cpp:2548) [PlayerLoop.cpp:498](C:/workflow/jnunity/Runtime/Misc/PlayerLoop.cpp:498) [GfxDeviceWorker.cpp:27](C:/workflow/jnunity/Runtime/GfxDevice/threaded/GfxDeviceWorker.cpp:27)

历史 Main.PlayerLoop 和 Render Thread 嵌套修复证明了边界维护的重要性。后续增加 direct scope 时，需要同时检查正常退出、早退、异常、重连、不同 producer 的 Begin/End 顺序。

## 4 Job System

ujobs 中保存与 handle/generation 关联的 Trace 身份，在调度、Ready、Dispatch、执行片、Steal、等待、SpinYield、ActiveHelp 等位置发送结构化阶段。JobBridge 负责配置、span identity、flow 和当前上下文。等待线程帮助执行任务时仍是执行工作，不能把整个等待容器都算为阻塞。[ujobs.cpp:139](C:/workflow/jnunity/Runtime/Jobs/Internal/JobQueues/ujobs/ujobs.cpp:139) [JNTracyJobBridge.cpp:160](C:/workflow/jnunity/Runtime/Profiler/JNTracyJobBridge.cpp:160)

维护检查包括：handle 重用时 generation 正确；旧连接任务不能错误进入新 capture；跨 worker 的 range/slice 与完整任务有清晰关系；Job 类型名与实际实例不混用；完成端未录到时返回缺口，不推断依赖已确认。

## 5 D3D12 与 DXR

| 模块 | 负责的信息 | 主要入口 |
|---|---|---|
| GfxBridge | CommandList、Submission、Gfx entity 与 CPU/Job/Frame 关系 | [JNTracyGfxBridge.cpp:1](C:/workflow/jnunity/Runtime/Profiler/JNTracyGfxBridge.cpp:1) |
| JNTracyD3D12Bridge | Direct/Compute/Copy context、Zone 和底层计时桥接 | [JNTracyD3D12Bridge.cpp:1](C:/workflow/jnunity/Runtime/GfxDevice/d3d12/JNTracyD3D12Bridge.cpp:1) |
| D3D12CommandList | GPU scope 生命周期、资源访问、flush/reset 边界 | [D3D12CommandList.cpp:12](C:/workflow/jnunity/Runtime/GfxDevice/d3d12/D3D12CommandList.cpp:12) |
| GfxDeviceDXR | DXR 原生 GPU 操作区间与相关证据 | [GfxDeviceDXR.cpp:67](C:/workflow/jnunity/Runtime/GfxDevice/d3d12/DXR/GfxDeviceDXR.cpp:67) |
| D3D12RayTracingState | SBT、资源访问及语义审计入口 | [D3D12RayTracingState.cpp:15](C:/workflow/jnunity/Runtime/GfxDevice/d3d12/DXR/D3D12RayTracingState.cpp:15) |
| Client D3D12 | C ABI 到 Tracy D3D12 计时实现 | [JNTracyD3D12.cpp:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyD3D12.cpp:1) |

GPU scope 必须跟随实际 CommandList 执行顺序。命令列表中途 flush、设备重建、连接变化、异步 Queue 和无显式 Pass 的 L0 都是需要独立覆盖的边界。GPU L0/Segment 序号不能当作 Player.Frame。

## 6 显存和资源

引擎采集的资源事实包含物理分配、Heap、逻辑资源、大小/类别、生命周期、驻留状态，以及 Frame/Pass 引用。DXGI 提供进程用量和预算；它与引擎注册表覆盖范围不同。新分类支持 Texture、RenderTarget、DepthStencil、Buffer、光追等用途。[D3D12MemoryManager.cpp:7](C:/workflow/jnunity/Runtime/GfxDevice/d3d12/D3D12MemoryManager.cpp:7) [JNTracyMemory.cpp:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyMemory.cpp:1)

Catalog 单独维护资源、视图、范围与关联等更丰富事实，具有自己的容量和失败状态。默认 INI 关闭 Catalog 后，GPU 内存和帧引用仍可能存在；“Catalog 关闭”不能解释为“没有任何 GPU 资源数据”。[JNTracyGpuCatalog.cpp:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyGpuCatalog.cpp:1) [配置矩阵](appendices/configuration-matrix.zh-CN.md)

Managed Catalog 可能通过 NativePointer 查询引入同步成本；是否开启要依据本次采集目的与有效配置。历史性能数据只能用于说明已发现的风险，不能作为新引擎版本的固定开销结论。

## 7 内存 I/O 系统使用率

Native CPU allocation 是选择性观测：当前 producer 描述含指定 MemLabel、至少 256 KiB 的条件；不能用这些记录计算整个进程全部分配总量。AsyncRead、JNFS、EncodedReader 和上传路径补充请求、阶段和 parent identity；网络聚合有独立启用与延期边界。[MemoryManager.cpp:12](C:/workflow/jnunity/Runtime/Allocator/MemoryManager.cpp:12) [AsyncReadManagerThreaded.cpp:9](C:/workflow/jnunity/Runtime/File/AsyncReadManagerThreaded.cpp:9) [JNTracyProfilerBridge.cpp:1816](C:/workflow/jnunity/Runtime/Profiler/JNTracyProfilerBridge.cpp:1816)

Usage 模块提供 CPU、内存、GPU 使用率等采样。采样曲线有时间间隔，其数值不等于某一帧的精确利用率；缺少 Sampling/ContextSwitch 数据时也不能根据请求开关补造值。[JNTracyUsage.cpp:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyUsage.cpp:1)

## 8 最新热路径与崩溃修复

当前 Writer 允许线程直接写入固定记录与时间戳，再由 Client 处理。接口包含 MarkerBegin、EndSource、LateZone 等类型，LateZone 保存原始开始和结束时间。它减少常规记录的跨层成本，但使事件排序、延后文本与关闭顺序成为关键维护点。[JNTracyWriter.h:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Public/JNTracyWriter.h:1) [JNTracyClient.cpp:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyClient.cpp:1)

10 月 12 日的 Client 补丁案例修复了 Zone 关闭顺序和文本关联。该案例只替换 Client DLL/PDB；整个合并还包含其他引擎变化，因此“一个 cpp 修复”不能自动推广为整个引擎升级都只换这一个 DLL。[验证案例](14-validation-and-real-cases.zh-CN.md)

## 9 修改后的验证顺序

1. 固定源码与实际修改范围，识别 ABI、序列化、Managed binding 是否改变。
2. 用 Native Harness/行为测试验证生命周期、连接代际、顺序和失败路径。
3. 构建独立 Client；涉及 Unity 主程序时构建对应 Editor/Player。
4. 在同源真实 Player 录制短样本，检查帧、层级、调用栈、资源关系和质量计数。
5. 若改变热路径，再做同条件开销测试；功能正确与性能开销单独给结论。

引擎构建与替换的完整步骤见 [构建部署](13-build-deploy-and-reproduce.zh-CN.md)。
