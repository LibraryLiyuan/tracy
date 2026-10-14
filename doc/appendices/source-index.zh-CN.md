# 源码与证据入口索引

入口绑定本轮源文件 SHA、行号和可选表达式。文件级入口从首行开始；本表是维护导航，不表示每个文件已逐行评审。

[机器登记](../evidence/source-registry.json) 保存完整身份。

## tracy

| 入口 | 文件与定位 | SHA 前缀 |
|---|---|---|
| capture | [capture.cpp:1](../../capture/src/capture.cpp#L1) | `1eeee29887e809f2` |
| capture-controller | [Controller.cpp:244](../../capture-gui/src/Controller.cpp#L244) | `7984814ffc20b025` |
| capture-process | [Process.cpp:1](../../capture-gui/src/Process.cpp#L1) | `bd514fc343c198ad` |
| capture-settings | [Core.hpp:42](../../capture-gui/src/Core.hpp#L42) | `d70a61eb28883c08` |
| journal | [TracyStreamJournal.cpp:1](../../stream/src/TracyStreamJournal.cpp#L1) | `ba1c3430a6512e90` |
| event-store | [TracyEventStore.hpp:14](../../stream/src/TracyEventStore.hpp#L14) | `913f23b5d3554c3c` |
| event-store-impl | [TracyEventStore.cpp:1](../../stream/src/TracyEventStore.cpp#L1) | `bc4f505a06d09311` |
| event-export | [TracyEventStoreExport.inl:1](../../stream/src/TracyEventStoreExport.inl#L1) | `f39f62e65fde7cbf` |
| snapshot-validation | [TracySnapshotValidate.inl:1](../../stream/src/TracySnapshotValidate.inl#L1) | `c282886b92cb3691` |
| sessions | [TracySessionManager.cpp:81](../../query/src/TracySessionManager.cpp#L81) | `9eeeec7a37047cc9` |
| event-source | [TracyEventStoreTraceSource.cpp:67](../../query/src/TracyEventStoreTraceSource.cpp#L67) | `37afbe021f5b0f49` |
| segment-source | [TracySegmentTraceSource.cpp:1](../../query/src/TracySegmentTraceSource.cpp#L1) | `3f6fd22718c7882f` |
| query-index | [TracyQueryIndex.cpp:1](../../query/src/TracyQueryIndex.cpp#L1) | `e0f88597ce57375f` |
| query-service | [TracyQueryService.cpp:1](../../query/src/TracyQueryService.cpp#L1) | `ef2137c996599dbf` |
| mcp | [TracyMcpServer.cpp:1](../../query/src/TracyMcpServer.cpp#L1) | `1b286ef700a04d67` |
| analyze | [analyze.cpp:249](../../query/src/analyze.cpp#L249) | `ee1b20ea87b8c028` |
| analyze-budget | [analyze.cpp:296](../../query/src/analyze.cpp#L296) | `ee1b20ea87b8c028` |
| analyze-fast | [TracyAnalyzeFast.hpp:1](../../query/src/TracyAnalyzeFast.hpp#L1) | `b14535fafa5bf13a` |
| triage | [TracyFrameTriage.cpp:1](../../analysis/TracyFrameTriage.cpp#L1) | `a8be547fb1ca10fa` |
| triage-header | [TracyFrameTriage.hpp:1](../../analysis/TracyFrameTriage.hpp#L1) | `fcbeefcbabca0f3f` |
| analysis-scan | [TracyAnalysisScanManager.cpp:1](../../query/src/TracyAnalysisScanManager.cpp#L1) | `b7ba8c1e0207d34a` |
| streaming-view | [TracyStreamingView.cpp:1](../../profiler/src/profiler/TracyStreamingView.cpp#L1) | `0129249a0f3e6a26` |
| streaming-timeline | [TracyStreamingTimeline.cpp:1](../../profiler/src/profiler/TracyStreamingTimeline.cpp#L1) | `4e80038e8b22b697` |
| report-navigation | [TracyView_ReportNavigation.cpp:1](../../profiler/src/profiler/TracyView_ReportNavigation.cpp#L1) | `e6c559885fcd280c` |
| profiler-main | [main.cpp:1](../../profiler/src/main.cpp#L1) | `bab0447c539f131a` |
| protocol | [TracyProtocol.hpp:12](../../public/common/TracyProtocol.hpp#L12) | `7236e3beada2030f` |
| version | [TracyVersion.hpp:1](../../public/common/TracyVersion.hpp#L1) | `9c9d8627cdf30f37` |
| query-cmake | [CMakeLists.txt:1](../../query/CMakeLists.txt#L1) | `ea04377e1bb2510c` |
| profiler-cmake | [CMakeLists.txt:1](../../profiler/CMakeLists.txt#L1) | `bbd3366ae039d994` |
| capture-cmake | [CMakeLists.txt:1](../../capture/CMakeLists.txt#L1) | `64e725719177e2aa` |
| skill-v2 | [SKILL.md:1](../../skills/diagnose-jn-unity-tracyV2/SKILL.md#L1) | `c9e96b38cb68b94b` |
| skill-stream | [stream-analysis.md:1](../../skills/diagnose-jn-unity-tracyV2/references/stream-analysis.md#L1) | `e125e0aedce780b2` |
| skill-cause | [cause-investigation.md:1](../../skills/diagnose-jn-unity-tracyV2/references/cause-investigation.md#L1) | `57fd1ab370f57eea` |
| skill-views | [stream-native-views.md:1](../../skills/diagnose-jn-unity-tracyV2/references/stream-native-views.md#L1) | `5192e4172b95d54b` |
| skill-old-flow | [query-native-scan-workflow.md:1](../../skills/diagnose-jn-unity-tracyV2/references/query-native-scan-workflow.md#L1) | `1f8afb474471214f` |
| skill-stream-workflow | [stream_workflow.py:1](../../skills/diagnose-jn-unity-tracyV2/scripts/stream_workflow.py#L1) | `b0ef9852765d83b8` |
| skill-review | [stream_review.py:1](../../skills/diagnose-jn-unity-tracyV2/scripts/stream_review.py#L1) | `509776b375d73540` |
| skill-views-wrapper | [stream_views.py:1](../../skills/diagnose-jn-unity-tracyV2/scripts/stream_views.py#L1) | `69c00f07764a90bc` |
| skill-install | [install-skill.ps1:1](../../skills/diagnose-jn-unity-tracyV2/scripts/install-skill.ps1#L1) | `a2e22adf22488556` |
| skill-profile | [stream-profile.json:1](../../skills/diagnose-jn-unity-tracyV2/config/stream-profile.json#L1) | `b558db15748ddd0e` |
| skill-profile-example | [stream-local.example.json:1](../../skills/diagnose-jn-unity-tracyV2/config/stream-local.example.json#L1) | `965e8b2caea1acf5` |
| viewer-server | [server.py:1](../../skills/diagnose-jn-unity-tracyV2/tools/stream-report-viewer/server.py#L1) | `d229eb17bc5b8915` |
| viewer-prepare | [prepare.py:1](../../skills/diagnose-jn-unity-tracyV2/tools/stream-report-viewer/prepare.py#L1) | `23dcb921884aedd6` |
| viewer-export | [export.py:1](../../skills/diagnose-jn-unity-tracyV2/tools/stream-report-viewer/export.py#L1) | `675833adf2c3ec3c` |
| viewer-start | [start.py:1](../../skills/diagnose-jn-unity-tracyV2/tools/stream-report-viewer/start.py#L1) | `614f23dd946465d0` |
| viewer-web | [viewer.html:1](../../skills/diagnose-jn-unity-tracyV2/tools/stream-report-viewer/viewer.html#L1) | `82d7d123db239c3f` |
| viewer-readme | [README.md:1](../../skills/diagnose-jn-unity-tracyV2/tools/stream-report-viewer/README.md#L1) | `114a041ef0cb8068` |
| skill-legacy | [SKILL.md:1](../../skills/diagnose-jn-unity-tracy/SKILL.md#L1) | `294e347146efb978` |

## jnunity

| 入口 | 文件与定位 | SHA 前缀 |
|---|---|---|
| engine-api | [JNTracyApi.h:22](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Public/JNTracyApi.h:22) | `cb31c806c644793b` |
| engine-writer | [JNTracyWriter.h:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Public/JNTracyWriter.h:1) | `97a8f4f9efbc9c1e` |
| engine-client | [JNTracyClient.cpp:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyClient.cpp:1) | `9cedf3da3215e516` |
| engine-gpu-native | [JNTracyD3D12.cpp:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyD3D12.cpp:1) | `23c258d7771da9c3` |
| engine-catalog | [JNTracyGpuCatalog.cpp:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyGpuCatalog.cpp:1) | `2e79930201d0e23a` |
| engine-memory | [JNTracyMemory.cpp:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyMemory.cpp:1) | `49f6a58b41129a96` |
| engine-usage | [JNTracyUsage.cpp:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Private/JNTracyUsage.cpp:1) | `00705cb2b00161e3` |
| engine-bridge | [JNTracyProfilerBridge.cpp:1816](C:/workflow/jnunity/Runtime/Profiler/JNTracyProfilerBridge.cpp:1816) | `a5679e3c69d332f3` |
| engine-marker | [Profiler.cpp:78](C:/workflow/jnunity/Runtime/Profiler/Profiler.cpp:78) | `46b97f6937f820b8` |
| engine-marker-registration | [ProfilerManager.cpp:249](C:/workflow/jnunity/Runtime/Profiler/ProfilerManager.cpp:249) | `cf3694fd573c8a72` |
| engine-job | [JNTracyJobBridge.cpp:160](C:/workflow/jnunity/Runtime/Profiler/JNTracyJobBridge.cpp:160) | `003dc456caae3d12` |
| engine-ujobs | [ujobs.cpp:139](C:/workflow/jnunity/Runtime/Jobs/Internal/JobQueues/ujobs/ujobs.cpp:139) | `47b650e0094b2639` |
| engine-gfx | [JNTracyGfxBridge.cpp:1](C:/workflow/jnunity/Runtime/Profiler/JNTracyGfxBridge.cpp:1) | `27db5a1222072621` |
| engine-d3d12 | [JNTracyD3D12Bridge.cpp:1](C:/workflow/jnunity/Runtime/GfxDevice/d3d12/JNTracyD3D12Bridge.cpp:1) | `84b97b7ddcd5bfec` |
| engine-command-list | [D3D12CommandList.cpp:12](C:/workflow/jnunity/Runtime/GfxDevice/d3d12/D3D12CommandList.cpp:12) | `37a353c74952e0ea` |
| engine-dxr | [GfxDeviceDXR.cpp:67](C:/workflow/jnunity/Runtime/GfxDevice/d3d12/DXR/GfxDeviceDXR.cpp:67) | `e1db3f1a3cf45928` |
| engine-dxr-sbt | [D3D12RayTracingState.cpp:15](C:/workflow/jnunity/Runtime/GfxDevice/d3d12/DXR/D3D12RayTracingState.cpp:15) | `e4167a0954f9a753` |
| engine-d3d12-memory | [D3D12MemoryManager.cpp:7](C:/workflow/jnunity/Runtime/GfxDevice/d3d12/D3D12MemoryManager.cpp:7) | `a65baa3ef2f8bc5f` |
| engine-cpu-memory | [MemoryManager.cpp:12](C:/workflow/jnunity/Runtime/Allocator/MemoryManager.cpp:12) | `1fc9d80896b4d8ff` |
| engine-async-read | [AsyncReadManagerThreaded.cpp:9](C:/workflow/jnunity/Runtime/File/AsyncReadManagerThreaded.cpp:9) | `4c14cc9ce7ed2425` |
| engine-player | [PlayerLoop.cpp:498](C:/workflow/jnunity/Runtime/Misc/PlayerLoop.cpp:498) | `9a3f90710da0e726` |
| engine-editor | [Application.cpp:2548](C:/workflow/jnunity/Editor/Src/Application/Application.cpp:2548) | `a648e8bce5d10331` |
| engine-render | [GfxDeviceWorker.cpp:27](C:/workflow/jnunity/Runtime/GfxDevice/threaded/GfxDeviceWorker.cpp:27) | `bf6e4459c1f1e6db` |
| engine-ini | [JNTracyIniConfig.cpp:1](C:/workflow/jnunity/Runtime/Profiler/JNTracyIniConfig.cpp:1) | `637e303490806ead` |
| engine-default-ini | [JNTracy.ini:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config/JNTracy.ini:1) | `6978cdee75c05b37` |
| engine-high-ini | [JNTracy.HighEvidence.ini:1](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config/JNTracy.HighEvidence.ini:1) | `223fc82829021b0c` |
| engine-build | [JNTracyClient.jam.cs:16](C:/workflow/jnunity/Projects/Jam/JNTracyClient.jam.cs:16) | `50f08790d42b752e` |
| engine-runtime-files | [RuntimeFiles.jam.cs:245](C:/workflow/jnunity/Projects/Jam/RuntimeFiles.jam.cs:245) | `a40ce5d182134748` |
| engine-global-defines | [GlobalDefines.jam.cs:108](C:/workflow/jnunity/Configuration/GlobalDefines.jam.cs:108) | `bf4bea14b2a8578c` |

## PackageRepo

| 入口 | 文件与定位 | SHA 前缀 |
|---|---|---|
| pack-runtime | [JNTracyRuntime.cs:124](C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTracyRuntime.cs:124) | `3dc6eb10856996ac` |
| pack-native | [JNTracyNative.cs:1](C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTracyNative.cs:1) | `983b8ed5f84bf593` |
| pack-marker | [JNTraceMarker.cs:123](C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceMarker.cs:123) | `36c248dbebb656d9` |
| pack-source-stack | [JNTraceSourceStack.cs:1](C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceSourceStack.cs:1) | `a6033a1ca295b367` |
| pack-ilpp | [JNTraceSourceStackILPostProcessor.cs:1](C:/workflow/PackageRepo/com.jngame.tracy/Editor/CodeGen/JNTraceSourceStackILPostProcessor.cs:1) | `6199ee9821cc2fa9` |
| pack-gpu-marker | [JNTraceGpuPassMarker.cs:1](C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceGpuPassMarker.cs:1) | `7663da96df8b42ba` |
| pack-gpu-registry | [JNRPGpuPassRegistry.cs:1](C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderPipeline/JNRPGpuPassRegistry.cs:1) | `1b2fdca32d9b8b27` |
| pack-profiling-scope | [ProfilingScope.cs:21](C:/workflow/PackageRepo/com.unity.render-pipelines.core/Runtime/RenderPipeline/ProfilingScope.cs:21) | `ab2c319fcf64744b` |
| pack-catalog | [JNTraceGpuCatalog.cs:1](C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceGpuCatalog.cs:1) | `257b5452821ce4dc` |
| pack-rdg-texture | [RDGResourceTexture.cs:334](C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGResourceTexture.cs:334) | `518c6f14705d1130` |
| pack-rdg-buffer | [RDGResourceComputeBuffer.cs:138](C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGResourceComputeBuffer.cs:138) | `af6a53d477802135` |
| pack-rdg-resources | [RDGResources.cs:1](C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGResources.cs:1) | `1177bef411212c71` |
| pack-vg | [JNVirtualGeometryTracyCatalog.cs:1](C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/JNVirtualGeometryTracyCatalog.cs:1) | `d0decfff01605381` |
| pack-pipeline | [JNRenderPipeline.cs:85](C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderPipeline/JNRenderPipeline.cs:85) | `8e02817cb695265d` |
| pack-frame-image | [JNTraceFrameImage.cs:1](C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceFrameImage.cs:1) | `eae714016f263a79` |
| pack-io | [JNTraceIO.cs:1](C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceIO.cs:1) | `253d27b18226db8e` |
| pack-load | [ResourceSystem.LoadTaskQueue.cs:95](C:/workflow/PackageRepo/com.jngame.assets-pipeline/Runtime/ResourceSystem/ResourceSystem.LoadTaskQueue.cs:95) | `167c652ab26b8fce` |
| pack-load-cancel | [ResourceSystem.cs:126](C:/workflow/PackageRepo/com.jngame.assets-pipeline/Runtime/ResourceSystem/ResourceSystem.cs:126) | `1ff437809054c229` |
| pack-editor-login | [JNTracyT3AutoLogin.cs:1](C:/workflow/PackageRepo/com.jngame.tracy/Editor/JNTracyT3AutoLogin.cs:1) | `5b15901501b3fe72` |
| pack-player-login | [JNTracyT3PlayerAutoLogin.cs:1](C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTracyT3PlayerAutoLogin.cs:1) | `e9dc52001f7a33dd` |
| pack-auto-capture | [Invoke-JNTracyPlayerAutoCapture.ps1:1](C:/workflow/PackageRepo/com.jngame.tracy/Tools/Invoke-JNTracyPlayerAutoCapture.ps1:1) | `3f7712e8b3e99ec9` |
| pack-asmdef | [JNGame.Tracy.asmdef:1](C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNGame.Tracy.asmdef:1) | `34e717bd3bd164a1` |

## stage-skill

| 入口 | 文件与定位 | SHA 前缀 |
|---|---|---|
| stage-skill | [SKILL.md:1](C:/Users/Admin/.codex/skills/jntracy-stage-player-code/SKILL.md:1) | `2a88129dd3a36527` |

## installed

| 入口 | 文件与定位 | SHA 前缀 |
|---|---|---|
| installed-build | [tracy-analyze-build.json:1](C:/Users/Admin/.codex/skills/diagnose-jn-unity-tracy/bin/tracy-analyze-build.json:1) | `d64eb0422ff286da` |

## workspace

| 入口 | 文件与定位 | SHA 前缀 |
|---|---|---|
| case-quality | [Tracy分析与报告质量深度评审.md:1](C:/Users/Admin/Documents/JN-Unity-T3/Skill-Quality-Review-20261008/Tracy分析与报告质量深度评审.md:1) | `5e7186e1b83e513f` |
| case-instrumentation | [插桩分析与报告格式约束建议.md:1](C:/Users/Admin/Documents/JN-Unity-T3/Skill-Quality-Review-20261008/插桩分析与报告格式约束建议.md:1) | `f08189eb2f82e163` |
| case-r20 | [Report.zh-CN.md:1](C:/Users/Admin/Documents/JN-Unity-T3/GpuCatalog-DiskIndex/Query-Final-Acceptance-20260929-r20/Report.zh-CN.md:1) | `8f4e35b550b2fa12` |
| case-fixed-slice | [README.md:1](C:/Users/Admin/Documents/JN-Unity-T3/Tracy-Fixed-Slice-Evidence/20261007-mcp-parity-001/README.md:1) | `6fc5f9169e142d14` |
| case-auto-login | [Player-T3-Auto-Login-and-Capture-Acceptance-2026-08-16.md:1](C:/Users/Admin/Documents/JN-Unity-T3/deployment-records/20260816/Player-T3-Auto-Login-and-Capture-Acceptance-2026-08-16.md:1) | `d64886d3f9347687` |
| case-worker | [SRPBatcher-Hierarchy-Audit.zh-CN.md:1](C:/Users/Admin/Documents/JN-Unity-T3/UnityProfiler-Tracy-JobWorker-20260928/SRPBatcher-Hierarchy-Audit.zh-CN.md:1) | `1ac4a383f2b23196` |
| case-full-skill | [README.md:1](C:/Users/Admin/Documents/JN-Unity-T3/Analysis/T3-115721-Full-SKILL-20261013/README.md:1) | `2344857f1f2854f2` |
| case-cost | [分析耗时与内存复盘.md:1](C:/Users/Admin/Documents/JN-Unity-T3/Analysis/T3-115721-cost-review-20261013/分析耗时与内存复盘.md:1) | `89627eea634528a2` |
| case-comparison | [两次性能分析结果对比.md:1](C:/Users/Admin/Documents/JN-Unity-T3/Share/T3两次性能分析结果对比_20261013/两次性能分析结果对比.md:1) | `470a3c5730e9405f` |
| case-player | [deployment-result.json:1](C:/Users/Admin/Documents/JN-Unity-T3/artifacts/client-patch-20261012-115800-c0cde1b6/deployment-result.json:1) | `c5b33ea2584dadf7` |
