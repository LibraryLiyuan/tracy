# Unity 引擎完整文件差异

目标 `feature/JNTracyUnity` / `c2877d4b053c3e084b0316fc08bc6a1209e2baa8`。比较 `81d9904a2504dd2a17691b4c0cf3741e33509f37..c2877d4b053c3e084b0316fc08bc6a1209e2baa8`，参考 `origin/T3/unity-2022.3.67f1`。本页穷尽文件级差异，正文重点解释生产主链，工作区变化另记。

| 分类 | 文件数 | 新增行 | 删除行 |
|---|---:|---:|---:|
| 构建与程序集 | 5 | 151 | 1 |
| 其他 | 1 | 1 | 0 |
| 实现代码与工具 | 70 | 24887 | 98 |
| 第三方与嵌入来源 | 84 | 53201 | 0 |
| 配置与数据合同 | 10 | 956 | 0 |
| 文档与图片 | 2 | 62 | 0 |
| 测试与验收工具 | 82 | 15052 | 0 |

机器清单：[CSV](../evidence/jnunity-changes.csv) · [JSON](../evidence/jnunity-changes.json)。第三方、测试、配置与元数据包含在总数内，新增行数不等于原创功能代码量。

## 构建与程序集

| 文件 | 变化 | 新增 | 删除 |
|---|---|---:|---:|
| [Configuration/GlobalDefines.jam.cs](<C:/workflow/jnunity/Configuration/GlobalDefines.jam.cs>) | M | 1 | 0 |
| [Jamfile.jam.cs](<C:/workflow/jnunity/Jamfile.jam.cs>) | M | 3 | 0 |
| [Projects/Jam/JNTracyClient.jam.cs](<C:/workflow/jnunity/Projects/Jam/JNTracyClient.jam.cs>) | A | 126 | 0 |
| [Projects/Jam/RuntimeFiles.jam.cs](<C:/workflow/jnunity/Projects/Jam/RuntimeFiles.jam.cs>) | M | 9 | 1 |
| [Tools/Unity.BuildSystem/Unity.BuildSystem/V2/WinEditor.cs](<C:/workflow/jnunity/Tools/Unity.BuildSystem/Unity.BuildSystem/V2/WinEditor.cs>) | M | 12 | 0 |

## 其他

| 文件 | 变化 | 新增 | 删除 |
|---|---|---:|---:|
| [Documentation/TrackedApi/editor/UnityEngine.CoreModule.api](<C:/workflow/jnunity/Documentation/TrackedApi/editor/UnityEngine.CoreModule.api>) | M | 1 | 0 |

## 实现代码与工具

| 文件 | 变化 | 新增 | 删除 |
|---|---|---:|---:|
| [Editor/Src/Application/Application.cpp](<C:/workflow/jnunity/Editor/Src/Application/Application.cpp>) | M | 7 | 0 |
| [Modules/JNFileSystem/Public/AssetLoadOperation.cpp](<C:/workflow/jnunity/Modules/JNFileSystem/Public/AssetLoadOperation.cpp>) | M | 39 | 1 |
| [Modules/JNFileSystem/Public/AssetLoadOperation.h](<C:/workflow/jnunity/Modules/JNFileSystem/Public/AssetLoadOperation.h>) | M | 2 | 0 |
| [Modules/Profiler/Public/Profiler.cpp](<C:/workflow/jnunity/Modules/Profiler/Public/Profiler.cpp>) | M | 5 | 0 |
| [PlatformDependent/WinPlayer/WinStandaloneSupport.cs](<C:/workflow/jnunity/PlatformDependent/WinPlayer/WinStandaloneSupport.cs>) | M | 11 | 0 |
| [Runtime/Allocator/MemoryManager.cpp](<C:/workflow/jnunity/Runtime/Allocator/MemoryManager.cpp>) | M | 18 | 0 |
| [Runtime/File/AsyncReadManager.h](<C:/workflow/jnunity/Runtime/File/AsyncReadManager.h>) | M | 14 | 0 |
| [Runtime/File/AsyncReadManagerThreaded.cpp](<C:/workflow/jnunity/Runtime/File/AsyncReadManagerThreaded.cpp>) | M | 95 | 0 |
| [Runtime/GfxDevice/ExternalGraphicsProfiler.h](<C:/workflow/jnunity/Runtime/GfxDevice/ExternalGraphicsProfiler.h>) | M | 22 | 0 |
| [Runtime/GfxDevice/GfxDeviceSetup.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/GfxDeviceSetup.cpp>) | M | 5 | 0 |
| [Runtime/GfxDevice/d3d12/BufferD3D12.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/BufferD3D12.cpp>) | M | 99 | 0 |
| [Runtime/GfxDevice/d3d12/BufferD3D12.h](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/BufferD3D12.h>) | M | 29 | 0 |
| [Runtime/GfxDevice/d3d12/D3D12CommandList.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/D3D12CommandList.cpp>) | M | 972 | 5 |
| [Runtime/GfxDevice/d3d12/D3D12CommandList.h](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/D3D12CommandList.h>) | M | 199 | 2 |
| [Runtime/GfxDevice/d3d12/D3D12Context.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/D3D12Context.cpp>) | M | 11 | 0 |
| [Runtime/GfxDevice/d3d12/D3D12DeviceState.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/D3D12DeviceState.cpp>) | M | 29 | 0 |
| [Runtime/GfxDevice/d3d12/D3D12Fence.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/D3D12Fence.cpp>) | M | 9 | 0 |
| [Runtime/GfxDevice/d3d12/D3D12MemoryManager.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/D3D12MemoryManager.cpp>) | M | 697 | 1 |
| [Runtime/GfxDevice/d3d12/D3D12MemoryManager.h](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/D3D12MemoryManager.h>) | M | 57 | 2 |
| [Runtime/GfxDevice/d3d12/D3D12SwapChain.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/D3D12SwapChain.cpp>) | M | 18 | 0 |
| [Runtime/GfxDevice/d3d12/D3D12UploadOperation.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/D3D12UploadOperation.cpp>) | M | 33 | 0 |
| [Runtime/GfxDevice/d3d12/DXR/D3D12RayTracingState.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/DXR/D3D12RayTracingState.cpp>) | M | 127 | 3 |
| [Runtime/GfxDevice/d3d12/DXR/GfxDeviceDXR.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/DXR/GfxDeviceDXR.cpp>) | M | 628 | 11 |
| [Runtime/GfxDevice/d3d12/GfxDeviceD3D12.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/GfxDeviceD3D12.cpp>) | M | 76 | 5 |
| [Runtime/GfxDevice/d3d12/GfxDeviceD3D12.h](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/GfxDeviceD3D12.h>) | M | 35 | 2 |
| [Runtime/GfxDevice/d3d12/GfxTaskExecutorD3D12.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/GfxTaskExecutorD3D12.cpp>) | M | 45 | 1 |
| [Runtime/GfxDevice/d3d12/JNTracyD3D12Bridge.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/JNTracyD3D12Bridge.cpp>) | A | 2532 | 0 |
| [Runtime/GfxDevice/d3d12/JNTracyD3D12Bridge.h](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/JNTracyD3D12Bridge.h>) | A | 230 | 0 |
| [Runtime/GfxDevice/d3d12/RenderTextureD3D12.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/d3d12/RenderTextureD3D12.cpp>) | M | 24 | 0 |
| [Runtime/GfxDevice/threaded/GfxDeviceWorker.cpp](<C:/workflow/jnunity/Runtime/GfxDevice/threaded/GfxDeviceWorker.cpp>) | M | 200 | 0 |
| [Runtime/Graphics/AsyncUploadManager.cpp](<C:/workflow/jnunity/Runtime/Graphics/AsyncUploadManager.cpp>) | M | 33 | 1 |
| [Runtime/Graphics/CommandBuffer/RenderingCommandBuffer.cpp](<C:/workflow/jnunity/Runtime/Graphics/CommandBuffer/RenderingCommandBuffer.cpp>) | M | 5 | 0 |
| [Runtime/Graphics/ScriptableRenderLoop/ScriptableDrawRenderers.cpp](<C:/workflow/jnunity/Runtime/Graphics/ScriptableRenderLoop/ScriptableDrawRenderers.cpp>) | M | 5 | 2 |
| [Runtime/Graphics/ScriptableRenderLoop/ScriptableDrawShadows.cpp](<C:/workflow/jnunity/Runtime/Graphics/ScriptableRenderLoop/ScriptableDrawShadows.cpp>) | M | 6 | 3 |
| [Runtime/Graphics/ScriptableRenderLoop/ScriptableRenderContext.cpp](<C:/workflow/jnunity/Runtime/Graphics/ScriptableRenderLoop/ScriptableRenderContext.cpp>) | M | 18 | 0 |
| [Runtime/Graphics/Shadow/RenderShadowMap.cpp](<C:/workflow/jnunity/Runtime/Graphics/Shadow/RenderShadowMap.cpp>) | M | 5 | 2 |
| [Runtime/Jobs/Internal/JobQueues/ujobs/ujobs.cpp](<C:/workflow/jnunity/Runtime/Jobs/Internal/JobQueues/ujobs/ujobs.cpp>) | M | 323 | 24 |
| [Runtime/Jobs/ScriptBindings/JobsBindings.cpp](<C:/workflow/jnunity/Runtime/Jobs/ScriptBindings/JobsBindings.cpp>) | M | 6 | 4 |
| [Runtime/Misc/Player.cpp](<C:/workflow/jnunity/Runtime/Misc/Player.cpp>) | M | 4 | 0 |
| [Runtime/Misc/PlayerLoop.cpp](<C:/workflow/jnunity/Runtime/Misc/PlayerLoop.cpp>) | M | 9 | 0 |
| [Runtime/Network/Sockets.cpp](<C:/workflow/jnunity/Runtime/Network/Sockets.cpp>) | M | 5 | 0 |
| [Runtime/Profiler/JNTracy/Private/JNTracyClient.cpp](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyClient.cpp>) | A | 5708 | 0 |
| [Runtime/Profiler/JNTracy/Private/JNTracyD3D12.cpp](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyD3D12.cpp>) | A | 301 | 0 |
| [Runtime/Profiler/JNTracy/Private/JNTracyGpuCatalog.cpp](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyGpuCatalog.cpp>) | A | 2309 | 0 |
| [Runtime/Profiler/JNTracy/Private/JNTracyGpuCatalogBudget.h](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyGpuCatalogBudget.h>) | A | 88 | 0 |
| [Runtime/Profiler/JNTracy/Private/JNTracyGpuCatalogPageControl.h](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyGpuCatalogPageControl.h>) | A | 101 | 0 |
| [Runtime/Profiler/JNTracy/Private/JNTracyInternal.h](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyInternal.h>) | A | 28 | 0 |
| [Runtime/Profiler/JNTracy/Private/JNTracyMemory.cpp](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyMemory.cpp>) | A | 1661 | 0 |
| [Runtime/Profiler/JNTracy/Private/JNTracySystem.cpp](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracySystem.cpp>) | A | 218 | 0 |
| [Runtime/Profiler/JNTracy/Private/JNTracyUsage.cpp](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyUsage.cpp>) | A | 557 | 0 |
| [Runtime/Profiler/JNTracy/Public/JNTracyApi.h](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Public/JNTracyApi.h>) | A | 1854 | 0 |
| [Runtime/Profiler/JNTracy/Public/JNTracyGpuTaxonomy.h](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Public/JNTracyGpuTaxonomy.h>) | A | 79 | 0 |
| [Runtime/Profiler/JNTracy/Public/JNTracyWriter.h](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Public/JNTracyWriter.h>) | A | 128 | 0 |
| [Runtime/Profiler/JNTracy/Tools/Generate-JNTracyCaptureIdentity.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tools/Generate-JNTracyCaptureIdentity.ps1>) | A | 261 | 0 |
| [Runtime/Profiler/JNTracyGfxBridge.cpp](<C:/workflow/jnunity/Runtime/Profiler/JNTracyGfxBridge.cpp>) | A | 225 | 0 |
| [Runtime/Profiler/JNTracyGfxBridge.h](<C:/workflow/jnunity/Runtime/Profiler/JNTracyGfxBridge.h>) | A | 100 | 0 |
| [Runtime/Profiler/JNTracyIniConfig.cpp](<C:/workflow/jnunity/Runtime/Profiler/JNTracyIniConfig.cpp>) | A | 590 | 0 |
| [Runtime/Profiler/JNTracyIniConfig.h](<C:/workflow/jnunity/Runtime/Profiler/JNTracyIniConfig.h>) | A | 44 | 0 |
| [Runtime/Profiler/JNTracyJobBridge.cpp](<C:/workflow/jnunity/Runtime/Profiler/JNTracyJobBridge.cpp>) | A | 364 | 0 |
| [Runtime/Profiler/JNTracyJobBridge.h](<C:/workflow/jnunity/Runtime/Profiler/JNTracyJobBridge.h>) | A | 159 | 0 |
| [Runtime/Profiler/JNTracyPlatform.h](<C:/workflow/jnunity/Runtime/Profiler/JNTracyPlatform.h>) | A | 11 | 0 |
| [Runtime/Profiler/JNTracyProfilerBridge.cpp](<C:/workflow/jnunity/Runtime/Profiler/JNTracyProfilerBridge.cpp>) | A | 2700 | 0 |
| [Runtime/Profiler/JNTracyProfilerBridge.h](<C:/workflow/jnunity/Runtime/Profiler/JNTracyProfilerBridge.h>) | A | 461 | 0 |
| [Runtime/Profiler/JNTracyUnityMarkerData.h](<C:/workflow/jnunity/Runtime/Profiler/JNTracyUnityMarkerData.h>) | A | 139 | 0 |
| [Runtime/Profiler/Marker.cpp](<C:/workflow/jnunity/Runtime/Profiler/Marker.cpp>) | M | 2 | 5 |
| [Runtime/Profiler/Marker.h](<C:/workflow/jnunity/Runtime/Profiler/Marker.h>) | M | 10 | 1 |
| [Runtime/Profiler/Profiler.cpp](<C:/workflow/jnunity/Runtime/Profiler/Profiler.cpp>) | M | 47 | 7 |
| [Runtime/Profiler/ProfilerManager.cpp](<C:/workflow/jnunity/Runtime/Profiler/ProfilerManager.cpp>) | M | 22 | 3 |
| [Runtime/Profiler/ProfilerManager.h](<C:/workflow/jnunity/Runtime/Profiler/ProfilerManager.h>) | M | 3 | 2 |
| [Runtime/VirtualFileSystem/JNFileSystem/JNEncodedFileReader.cpp](<C:/workflow/jnunity/Runtime/VirtualFileSystem/JNFileSystem/JNEncodedFileReader.cpp>) | M | 30 | 11 |

## 第三方与嵌入来源

| 文件 | 变化 | 新增 | 删除 |
|---|---|---:|---:|
| [External/Tracy/JN_TRACY_REVISION.txt](<C:/workflow/jnunity/External/Tracy/JN_TRACY_REVISION.txt>) | A | 6 | 0 |
| [External/Tracy/LICENSE](<C:/workflow/jnunity/External/Tracy/LICENSE>) | A | 27 | 0 |
| [External/Tracy/public/TracyClient.F90](<C:/workflow/jnunity/External/Tracy/public/TracyClient.F90>) | A | 1292 | 0 |
| [External/Tracy/public/TracyClient.cpp](<C:/workflow/jnunity/External/Tracy/public/TracyClient.cpp>) | A | 45 | 0 |
| [External/Tracy/public/client/TracyAlloc.cpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyAlloc.cpp>) | A | 43 | 0 |
| [External/Tracy/public/client/TracyArmCpuTable.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyArmCpuTable.hpp>) | A | 419 | 0 |
| [External/Tracy/public/client/TracyCallstack.cpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyCallstack.cpp>) | A | 1481 | 0 |
| [External/Tracy/public/client/TracyCallstack.h](<C:/workflow/jnunity/External/Tracy/public/client/TracyCallstack.h>) | A | 39 | 0 |
| [External/Tracy/public/client/TracyCallstack.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyCallstack.hpp>) | A | 170 | 0 |
| [External/Tracy/public/client/TracyCpuid.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyCpuid.hpp>) | A | 12 | 0 |
| [External/Tracy/public/client/TracyDebug.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyDebug.hpp>) | A | 11 | 0 |
| [External/Tracy/public/client/TracyDxt1.cpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyDxt1.cpp>) | A | 644 | 0 |
| [External/Tracy/public/client/TracyDxt1.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyDxt1.hpp>) | A | 11 | 0 |
| [External/Tracy/public/client/TracyFastVector.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyFastVector.hpp>) | A | 118 | 0 |
| [External/Tracy/public/client/TracyJnClient.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyJnClient.hpp>) | A | 523 | 0 |
| [External/Tracy/public/client/TracyKCore.cpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyKCore.cpp>) | A | 121 | 0 |
| [External/Tracy/public/client/TracyKCore.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyKCore.hpp>) | A | 37 | 0 |
| [External/Tracy/public/client/TracyLock.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyLock.hpp>) | A | 548 | 0 |
| [External/Tracy/public/client/TracyOverride.cpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyOverride.cpp>) | A | 26 | 0 |
| [External/Tracy/public/client/TracyProfiler.cpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyProfiler.cpp>) | A | 5770 | 0 |
| [External/Tracy/public/client/TracyProfiler.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyProfiler.hpp>) | A | 1353 | 0 |
| [External/Tracy/public/client/TracyRingBuffer.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyRingBuffer.hpp>) | A | 141 | 0 |
| [External/Tracy/public/client/TracyRocprof.cpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyRocprof.cpp>) | A | 556 | 0 |
| [External/Tracy/public/client/TracyScoped.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyScoped.hpp>) | A | 207 | 0 |
| [External/Tracy/public/client/TracyStringHelpers.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyStringHelpers.hpp>) | A | 41 | 0 |
| [External/Tracy/public/client/TracySysPower.cpp](<C:/workflow/jnunity/External/Tracy/public/client/TracySysPower.cpp>) | A | 164 | 0 |
| [External/Tracy/public/client/TracySysPower.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracySysPower.hpp>) | A | 44 | 0 |
| [External/Tracy/public/client/TracySysTime.cpp](<C:/workflow/jnunity/External/Tracy/public/client/TracySysTime.cpp>) | A | 120 | 0 |
| [External/Tracy/public/client/TracySysTime.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracySysTime.hpp>) | A | 36 | 0 |
| [External/Tracy/public/client/TracySysTrace.cpp](<C:/workflow/jnunity/External/Tracy/public/client/TracySysTrace.cpp>) | A | 1997 | 0 |
| [External/Tracy/public/client/TracySysTrace.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracySysTrace.hpp>) | A | 35 | 0 |
| [External/Tracy/public/client/TracyThread.hpp](<C:/workflow/jnunity/External/Tracy/public/client/TracyThread.hpp>) | A | 103 | 0 |
| [External/Tracy/public/client/tracy_SPSCQueue.h](<C:/workflow/jnunity/External/Tracy/public/client/tracy_SPSCQueue.h>) | A | 148 | 0 |
| [External/Tracy/public/client/tracy_concurrentqueue.h](<C:/workflow/jnunity/External/Tracy/public/client/tracy_concurrentqueue.h>) | A | 1441 | 0 |
| [External/Tracy/public/client/tracy_rpmalloc.cpp](<C:/workflow/jnunity/External/Tracy/public/client/tracy_rpmalloc.cpp>) | A | 3519 | 0 |
| [External/Tracy/public/client/tracy_rpmalloc.hpp](<C:/workflow/jnunity/External/Tracy/public/client/tracy_rpmalloc.hpp>) | A | 363 | 0 |
| [External/Tracy/public/common/TracyAlign.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracyAlign.hpp>) | A | 27 | 0 |
| [External/Tracy/public/common/TracyAlloc.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracyAlloc.hpp>) | A | 72 | 0 |
| [External/Tracy/public/common/TracyApi.h](<C:/workflow/jnunity/External/Tracy/public/common/TracyApi.h>) | A | 16 | 0 |
| [External/Tracy/public/common/TracyColor.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracyColor.hpp>) | A | 690 | 0 |
| [External/Tracy/public/common/TracyForceInline.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracyForceInline.hpp>) | A | 20 | 0 |
| [External/Tracy/public/common/TracyJnGpuCatalog.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracyJnGpuCatalog.hpp>) | A | 416 | 0 |
| [External/Tracy/public/common/TracyJnUsage.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracyJnUsage.hpp>) | A | 77 | 0 |
| [External/Tracy/public/common/TracyJnVram.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracyJnVram.hpp>) | A | 93 | 0 |
| [External/Tracy/public/common/TracyMutex.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracyMutex.hpp>) | A | 24 | 0 |
| [External/Tracy/public/common/TracyProtocol.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracyProtocol.hpp>) | A | 172 | 0 |
| [External/Tracy/public/common/TracyQueue.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracyQueue.hpp>) | A | 1707 | 0 |
| [External/Tracy/public/common/TracySocket.cpp](<C:/workflow/jnunity/External/Tracy/public/common/TracySocket.cpp>) | A | 766 | 0 |
| [External/Tracy/public/common/TracySocket.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracySocket.hpp>) | A | 156 | 0 |
| [External/Tracy/public/common/TracyStackFrames.cpp](<C:/workflow/jnunity/External/Tracy/public/common/TracyStackFrames.cpp>) | A | 122 | 0 |
| [External/Tracy/public/common/TracyStackFrames.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracyStackFrames.hpp>) | A | 22 | 0 |
| [External/Tracy/public/common/TracySystem.cpp](<C:/workflow/jnunity/External/Tracy/public/common/TracySystem.cpp>) | A | 349 | 0 |
| [External/Tracy/public/common/TracySystem.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracySystem.hpp>) | A | 43 | 0 |
| [External/Tracy/public/common/TracyVersion.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracyVersion.hpp>) | A | 14 | 0 |
| [External/Tracy/public/common/TracyWinFamily.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracyWinFamily.hpp>) | A | 16 | 0 |
| [External/Tracy/public/common/TracyYield.hpp](<C:/workflow/jnunity/External/Tracy/public/common/TracyYield.hpp>) | A | 28 | 0 |
| [External/Tracy/public/common/tracy_lz4.cpp](<C:/workflow/jnunity/External/Tracy/public/common/tracy_lz4.cpp>) | A | 2720 | 0 |
| [External/Tracy/public/common/tracy_lz4.hpp](<C:/workflow/jnunity/External/Tracy/public/common/tracy_lz4.hpp>) | A | 847 | 0 |
| [External/Tracy/public/common/tracy_lz4hc.cpp](<C:/workflow/jnunity/External/Tracy/public/common/tracy_lz4hc.cpp>) | A | 1636 | 0 |
| [External/Tracy/public/common/tracy_lz4hc.hpp](<C:/workflow/jnunity/External/Tracy/public/common/tracy_lz4hc.hpp>) | A | 405 | 0 |
| [External/Tracy/public/libbacktrace/LICENSE](<C:/workflow/jnunity/External/Tracy/public/libbacktrace/LICENSE>) | A | 29 | 0 |
| [External/Tracy/public/libbacktrace/alloc.cpp](<C:/workflow/jnunity/External/Tracy/public/libbacktrace/alloc.cpp>) | A | 174 | 0 |
| [External/Tracy/public/libbacktrace/backtrace.hpp](<C:/workflow/jnunity/External/Tracy/public/libbacktrace/backtrace.hpp>) | A | 186 | 0 |
| [External/Tracy/public/libbacktrace/config.h](<C:/workflow/jnunity/External/Tracy/public/libbacktrace/config.h>) | A | 26 | 0 |
| [External/Tracy/public/libbacktrace/dwarf.cpp](<C:/workflow/jnunity/External/Tracy/public/libbacktrace/dwarf.cpp>) | A | 4455 | 0 |
| [External/Tracy/public/libbacktrace/elf.cpp](<C:/workflow/jnunity/External/Tracy/public/libbacktrace/elf.cpp>) | A | 7605 | 0 |
| [External/Tracy/public/libbacktrace/fileline.cpp](<C:/workflow/jnunity/External/Tracy/public/libbacktrace/fileline.cpp>) | A | 412 | 0 |
| [External/Tracy/public/libbacktrace/filenames.hpp](<C:/workflow/jnunity/External/Tracy/public/libbacktrace/filenames.hpp>) | A | 52 | 0 |
| [External/Tracy/public/libbacktrace/internal.hpp](<C:/workflow/jnunity/External/Tracy/public/libbacktrace/internal.hpp>) | A | 435 | 0 |
| [External/Tracy/public/libbacktrace/macho.cpp](<C:/workflow/jnunity/External/Tracy/public/libbacktrace/macho.cpp>) | A | 1367 | 0 |
| [External/Tracy/public/libbacktrace/mmapio.cpp](<C:/workflow/jnunity/External/Tracy/public/libbacktrace/mmapio.cpp>) | A | 115 | 0 |
| [External/Tracy/public/libbacktrace/posix.cpp](<C:/workflow/jnunity/External/Tracy/public/libbacktrace/posix.cpp>) | A | 109 | 0 |
| [External/Tracy/public/libbacktrace/sort.cpp](<C:/workflow/jnunity/External/Tracy/public/libbacktrace/sort.cpp>) | A | 113 | 0 |
| [External/Tracy/public/libbacktrace/state.cpp](<C:/workflow/jnunity/External/Tracy/public/libbacktrace/state.cpp>) | A | 76 | 0 |
| [External/Tracy/public/tracy/Tracy.hpp](<C:/workflow/jnunity/External/Tracy/public/tracy/Tracy.hpp>) | A | 280 | 0 |
| [External/Tracy/public/tracy/TracyC.h](<C:/workflow/jnunity/External/Tracy/public/tracy/TracyC.h>) | A | 395 | 0 |
| [External/Tracy/public/tracy/TracyCUDA.hpp](<C:/workflow/jnunity/External/Tracy/public/tracy/TracyCUDA.hpp>) | A | 1325 | 0 |
| [External/Tracy/public/tracy/TracyD3D11.hpp](<C:/workflow/jnunity/External/Tracy/public/tracy/TracyD3D11.hpp>) | A | 466 | 0 |
| [External/Tracy/public/tracy/TracyD3D12.hpp](<C:/workflow/jnunity/External/Tracy/public/tracy/TracyD3D12.hpp>) | A | 916 | 0 |
| [External/Tracy/public/tracy/TracyLua.hpp](<C:/workflow/jnunity/External/Tracy/public/tracy/TracyLua.hpp>) | A | 486 | 0 |
| [External/Tracy/public/tracy/TracyMetal.hmm](<C:/workflow/jnunity/External/Tracy/public/tracy/TracyMetal.hmm>) | A | 644 | 0 |
| [External/Tracy/public/tracy/TracyOpenCL.hpp](<C:/workflow/jnunity/External/Tracy/public/tracy/TracyOpenCL.hpp>) | A | 414 | 0 |
| [External/Tracy/public/tracy/TracyOpenGL.hpp](<C:/workflow/jnunity/External/Tracy/public/tracy/TracyOpenGL.hpp>) | A | 325 | 0 |
| [External/Tracy/public/tracy/TracyVulkan.hpp](<C:/workflow/jnunity/External/Tracy/public/tracy/TracyVulkan.hpp>) | A | 747 | 0 |

## 配置与数据合同

| 文件 | 变化 | 新增 | 删除 |
|---|---|---:|---:|
| [Runtime/Profiler/JNTracy/Config/JNTracy.AllDomainsStress.ini](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config/JNTracy.AllDomainsStress.ini>) | A | 101 | 0 |
| [Runtime/Profiler/JNTracy/Config/JNTracy.Daily.ini](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config/JNTracy.Daily.ini>) | A | 85 | 0 |
| [Runtime/Profiler/JNTracy/Config/JNTracy.HighEvidence.ini](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config/JNTracy.HighEvidence.ini>) | A | 128 | 0 |
| [Runtime/Profiler/JNTracy/Config/JNTracy.Triggered.ExactCallstack.ini](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config/JNTracy.Triggered.ExactCallstack.ini>) | A | 84 | 0 |
| [Runtime/Profiler/JNTracy/Config/JNTracy.Triggered.GpuReference.Periodic60.ini](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config/JNTracy.Triggered.GpuReference.Periodic60.ini>) | A | 101 | 0 |
| [Runtime/Profiler/JNTracy/Config/JNTracy.Triggered.GpuReference.ini](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config/JNTracy.Triggered.GpuReference.ini>) | A | 101 | 0 |
| [Runtime/Profiler/JNTracy/Config/JNTracy.Triggered.LuaCoroutine.ini](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config/JNTracy.Triggered.LuaCoroutine.ini>) | A | 69 | 0 |
| [Runtime/Profiler/JNTracy/Config/JNTracy.Triggered.StartupIOMemory.ini](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config/JNTracy.Triggered.StartupIOMemory.ini>) | A | 69 | 0 |
| [Runtime/Profiler/JNTracy/Config/JNTracy.Triggered.SystemSampling.ini](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config/JNTracy.Triggered.SystemSampling.ini>) | A | 69 | 0 |
| [Runtime/Profiler/JNTracy/Config/JNTracy.ini](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config/JNTracy.ini>) | A | 149 | 0 |

## 文档与图片

| 文件 | 变化 | 新增 | 删除 |
|---|---|---:|---:|
| [Runtime/Profiler/JNTracy/Config/README.zh-CN.md](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config/README.zh-CN.md>) | A | 47 | 0 |
| [Runtime/Profiler/JNTracy/README.md](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/README.md>) | A | 15 | 0 |

## 测试与验收工具

| 文件 | 变化 | 新增 | 删除 |
|---|---|---:|---:|
| [Runtime/Profiler/JNTracy/Tests/Analyze-JNTracyGpuReferenceAb.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Analyze-JNTracyGpuReferenceAb.ps1>) | A | 209 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyDirectGpuMcp.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyDirectGpuMcp.ps1>) | A | 429 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyDirectGpuPerformance.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyDirectGpuPerformance.ps1>) | A | 249 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyFrameImagePerformance.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyFrameImagePerformance.ps1>) | A | 265 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN11Performance.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN11Performance.ps1>) | A | 148 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN12JobPerformance.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN12JobPerformance.ps1>) | A | 167 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN13FileIoPerformance.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN13FileIoPerformance.ps1>) | A | 189 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN15PairedPerformance.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN15PairedPerformance.ps1>) | A | 504 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN15PairedPerformanceGate.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN15PairedPerformanceGate.ps1>) | A | 114 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN15Performance.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN15Performance.ps1>) | A | 393 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN15PerformanceGate.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN15PerformanceGate.ps1>) | A | 130 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16CaptureProfileMcp.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16CaptureProfileMcp.ps1>) | A | 348 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16ElevatedDetailSampling.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16ElevatedDetailSampling.ps1>) | A | 41 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16ElevatedDispatcher.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16ElevatedDispatcher.ps1>) | A | 462 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16HarnessReconnect.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16HarnessReconnect.ps1>) | A | 142 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16JobSchema3PairedPerformance.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16JobSchema3PairedPerformance.ps1>) | A | 322 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16ProtocolMcp.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16ProtocolMcp.ps1>) | A | 329 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16SamplingAnalysis.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16SamplingAnalysis.ps1>) | A | 311 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16SemanticAuditCapture.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16SemanticAuditCapture.ps1>) | A | 225 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16SemanticAuditMcp.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16SemanticAuditMcp.ps1>) | A | 247 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16SourceStackMcp.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16SourceStackMcp.ps1>) | A | 314 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16SourceStackPairedPerformance.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16SourceStackPairedPerformance.ps1>) | A | 464 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16WprCpuDifferential.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16WprCpuDifferential.ps1>) | A | 338 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16WprDifferentialTask.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN16WprDifferentialTask.ps1>) | A | 186 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN17FeaturePairedPerformance.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN17FeaturePairedPerformance.ps1>) | A | 198 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN17GpuReferencePairingAudit.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN17GpuReferencePairingAudit.ps1>) | A | 151 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN17MainTracePairedPerformance.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN17MainTracePairedPerformance.ps1>) | A | 137 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN17PresetCapture.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN17PresetCapture.ps1>) | A | 556 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN17StartupIoMemoryCapture.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN17StartupIoMemoryCapture.ps1>) | A | 251 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN17SystemSamplingPairedPerformance.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN17SystemSamplingPairedPerformance.ps1>) | A | 128 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN25CallsiteMcp.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN25CallsiteMcp.ps1>) | A | 258 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN25CallsitePairedPerformance.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN25CallsitePairedPerformance.ps1>) | A | 292 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN25CpuAllocationSiteReuseMcp.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN25CpuAllocationSiteReuseMcp.ps1>) | A | 175 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN27PeriodicAbRun.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Invoke-JNTracyN27PeriodicAbRun.ps1>) | A | 272 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyCallstackConfigTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyCallstackConfigTests.ps1>) | A | 135 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyDailyPresetStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyDailyPresetStaticTests.ps1>) | A | 214 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyDxrGpuTimingIntegrationTests.py](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyDxrGpuTimingIntegrationTests.py>) | A | 147 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyDxrPlayerLoadTests.py](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyDxrPlayerLoadTests.py>) | A | 22 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyFrameReferencesStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyFrameReferencesStaticTests.ps1>) | A | 65 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyGpuMarkerContinuationStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyGpuMarkerContinuationStaticTests.ps1>) | A | 58 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyHarness.cpp](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyHarness.cpp>) | A | 1828 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyN17JobCausalityStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyN17JobCausalityStaticTests.ps1>) | A | 79 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyN17ManagedLuaStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyN17ManagedLuaStaticTests.ps1>) | A | 77 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyN17NativeCpuStageStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyN17NativeCpuStageStaticTests.ps1>) | A | 52 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyN18FrameRootStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyN18FrameRootStaticTests.ps1>) | A | 54 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyN25CallsiteStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyN25CallsiteStaticTests.ps1>) | A | 73 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyN25SecondaryProducerStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyN25SecondaryProducerStaticTests.ps1>) | A | 110 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyN27CatalogQueueStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyN27CatalogQueueStaticTests.ps1>) | A | 100 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyN27CatalogSchemaStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyN27CatalogSchemaStaticTests.ps1>) | A | 86 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyN27D3D12CatalogStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyN27D3D12CatalogStaticTests.ps1>) | A | 45 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyN27IniAndCostStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyN27IniAndCostStaticTests.ps1>) | A | 59 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyN27RangeLogicalMeshStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyN27RangeLogicalMeshStaticTests.ps1>) | A | 52 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyN27VgRtasEvidenceStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyN27VgRtasEvidenceStaticTests.ps1>) | A | 72 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyN27WorkerStreamPersistenceStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyN27WorkerStreamPersistenceStaticTests.ps1>) | A | 57 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyOverheadAbStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyOverheadAbStaticTests.ps1>) | A | 88 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyOverheadTrimStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyOverheadTrimStaticTests.ps1>) | A | 87 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyRenderFrameRootStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyRenderFrameRootStaticTests.ps1>) | A | 55 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyRenderMarkerBoundaryStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyRenderMarkerBoundaryStaticTests.ps1>) | A | 61 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracySystemUsageStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracySystemUsageStaticTests.ps1>) | A | 84 | 0 |
| [Runtime/Profiler/JNTracy/Tests/JNTracyThreadCoresStaticTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/JNTracyThreadCoresStaticTests.ps1>) | A | 74 | 0 |
| [Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/CatalogBudgetDllBehavior.cpp.txt](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/CatalogBudgetDllBehavior.cpp.txt>) | A | 98 | 0 |
| [Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/MetadataBehavior.cpp.txt](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/MetadataBehavior.cpp.txt>) | A | 31 | 0 |
| [Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/NuGet.Config](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/NuGet.Config>) | A | 1 | 0 |
| [Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/README.md](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/README.md>) | A | 25 | 0 |
| [Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/Run-BehaviorTests.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/Run-BehaviorTests.ps1>) | A | 37 | 0 |
| [Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/ScopeHarness.cs.txt](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/ScopeHarness.cs.txt>) | A | 38 | 0 |
| [Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/ScopeHarness.csproj](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/ScopeHarness.csproj>) | A | 24 | 0 |
| [Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/native_test_support.py](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/native_test_support.py>) | A | 75 | 0 |
| [Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/run_callback_behavior.py](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/run_callback_behavior.py>) | A | 132 | 0 |
| [Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/run_catalog_budget_behavior.py](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/run_catalog_budget_behavior.py>) | A | 175 | 0 |
| [Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/run_completeness_boundaries.py](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/run_completeness_boundaries.py>) | A | 177 | 0 |
| [Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/run_native_behavior.py](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/run_native_behavior.py>) | A | 57 | 0 |
| [Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/run_simplification_behavior.py](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/run_simplification_behavior.py>) | A | 159 | 0 |
| [Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/test_static_marker_index.py](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/MarkerCompleteness/test_static_marker_index.py>) | A | 28 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Merge-JNTracyGpuReferenceAb.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Merge-JNTracyGpuReferenceAb.ps1>) | A | 103 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Merge-JNTracyN15PairedPerformanceGates.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Merge-JNTracyN15PairedPerformanceGates.ps1>) | A | 294 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Merge-JNTracyN17JobCallstackPerformance.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Merge-JNTracyN17JobCallstackPerformance.ps1>) | A | 123 | 0 |
| [Runtime/Profiler/JNTracy/Tests/New-JNTracyN16ElevatedRequest.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/New-JNTracyN16ElevatedRequest.ps1>) | A | 156 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Register-JNTracyN16ElevatedTasks.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Register-JNTracyN16ElevatedTasks.ps1>) | A | 193 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Register-JNTracyN16WprTask.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Register-JNTracyN16WprTask.ps1>) | A | 183 | 0 |
| [Runtime/Profiler/JNTracy/Tests/Test-JNTracyN16UnitySemanticIsolation.ps1](<C:/workflow/jnunity/Runtime/Profiler/JNTracy/Tests/Test-JNTracyN16UnitySemanticIsolation.ps1>) | A | 164 | 0 |
| [Runtime/Profiler/MemoryProfilerTests.cpp](<C:/workflow/jnunity/Runtime/Profiler/MemoryProfilerTests.cpp>) | M | 1 | 0 |
