# PackageRepo 完整文件差异

目标 `feature/Pack-JNTracy` / `0ac6735ce3ca84f7ffbe2dd0cfcfa8acbfa2ed91`。比较 `92f1ededb40cfada15df61411a5ad0a7d2ce12eb..0ac6735ce3ca84f7ffbe2dd0cfcfa8acbfa2ed91`，参考 `origin/t3/development`。本页穷尽文件级差异，正文重点解释生产主链，工作区变化另记。

| 分类 | 文件数 | 新增行 | 删除行 |
|---|---:|---:|---:|
| 构建与程序集 | 6 | 87 | 5 |
| 实现代码与工具 | 53 | 9913 | 37 |
| 配置与数据合同 | 3 | 20 | 2 |
| Unity 元数据 | 36 | 333 | 0 |
| 测试与验收工具 | 1 | 101 | 0 |

机器清单：[CSV](../evidence/PackageRepo-changes.csv) · [JSON](../evidence/PackageRepo-changes.json)。第三方、测试、配置与元数据包含在总数内，新增行数不等于原创功能代码量。

## 构建与程序集

| 文件 | 变化 | 新增 | 删除 |
|---|---|---:|---:|
| [com.jngame.assets-pipeline/Runtime/JNGame.AssetsPipeline.Runtime.asmdef](<C:/workflow/PackageRepo/com.jngame.assets-pipeline/Runtime/JNGame.AssetsPipeline.Runtime.asmdef>) | M | 10 | 3 |
| [com.jngame.render-pipelines/Runtime/JNGame.RenderPipelines.Runtime.asmdef](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/JNGame.RenderPipelines.Runtime.asmdef>) | M | 7 | 1 |
| [com.jngame.tracy/Editor/CodeGen/JNGame.Tracy.CodeGen.asmdef](<C:/workflow/PackageRepo/com.jngame.tracy/Editor/CodeGen/JNGame.Tracy.CodeGen.asmdef>) | A | 26 | 0 |
| [com.jngame.tracy/Editor/JNGame.Tracy.Editor.asmdef](<C:/workflow/PackageRepo/com.jngame.tracy/Editor/JNGame.Tracy.Editor.asmdef>) | A | 18 | 0 |
| [com.jngame.tracy/Runtime/JNGame.Tracy.asmdef](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNGame.Tracy.asmdef>) | A | 20 | 0 |
| [com.unity.render-pipelines.core/Runtime/Unity.RenderPipelines.Core.Runtime.asmdef](<C:/workflow/PackageRepo/com.unity.render-pipelines.core/Runtime/Unity.RenderPipelines.Core.Runtime.asmdef>) | M | 6 | 1 |

## 实现代码与工具

| 文件 | 变化 | 新增 | 删除 |
|---|---|---:|---:|
| [com.jngame.assets-pipeline/Runtime/ResourceSystem/ResourceSystem.LoadTaskQueue.cs](<C:/workflow/PackageRepo/com.jngame.assets-pipeline/Runtime/ResourceSystem/ResourceSystem.LoadTaskQueue.cs>) | M | 48 | 1 |
| [com.jngame.assets-pipeline/Runtime/ResourceSystem/ResourceSystem.cs](<C:/workflow/PackageRepo/com.jngame.assets-pipeline/Runtime/ResourceSystem/ResourceSystem.cs>) | M | 7 | 0 |
| [com.jngame.assets-pipeline/Runtime/ResourceSystem/Task/LoadAssetTask.cs](<C:/workflow/PackageRepo/com.jngame.assets-pipeline/Runtime/ResourceSystem/Task/LoadAssetTask.cs>) | M | 4 | 1 |
| [com.jngame.render-pipelines/Editor/Diagnostics/JNTracyGpuValidation.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Editor/Diagnostics/JNTracyGpuValidation.cs>) | A | 180 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/Component/HDWorld.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/Component/HDWorld.cs>) | M | 16 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGBuilder.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGBuilder.cs>) | M | 34 | 1 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGPassBuilder.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGPassBuilder.cs>) | M | 17 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGResourceAliasing.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGResourceAliasing.cs>) | M | 8 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGResourceComputeBuffer.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGResourceComputeBuffer.cs>) | M | 12 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGResourceFactory.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGResourceFactory.cs>) | M | 79 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGResourceTexture.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGResourceTexture.cs>) | M | 11 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGResources.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderGraph/RDGResources.cs>) | M | 96 | 1 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/RenderPasses/FinalBlitPass.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderPasses/FinalBlitPass.cs>) | M | 43 | 1 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/RenderPasses/ScreenProbePass.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderPasses/ScreenProbePass.cs>) | M | 5 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/RenderPasses/TSRPass.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderPasses/TSRPass.cs>) | M | 11 | 6 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/RenderPasses/UI/UIFinalBlitPass.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderPasses/UI/UIFinalBlitPass.cs>) | M | 39 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/RenderPipeline/JNRPGpuPassRegistry.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderPipeline/JNRPGpuPassRegistry.cs>) | M | 377 | 11 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/RenderPipeline/JNRenderPipeline.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/RenderPipeline/JNRenderPipeline.cs>) | M | 31 | 3 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/JNVirtualGeometryTracyCatalog.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/JNVirtualGeometryTracyCatalog.cs>) | A | 501 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/VGConsoleHWRasterPass.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/VGConsoleHWRasterPass.cs>) | M | 5 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/VGCullingAfterVSMPass.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/VGCullingAfterVSMPass.cs>) | M | 5 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/VGCullingPass.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/VGCullingPass.cs>) | M | 5 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/VGHizPass.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/VGHizPass.cs>) | M | 5 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/VGStreamingBurstHelper.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/VGStreamingBurstHelper.cs>) | M | 2 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/VGStreamingManager.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/VGStreamingManager.cs>) | M | 17 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/VirtualShadowMap/VSMBuildPageAllocationsPass.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/VirtualShadowMap/VSMBuildPageAllocationsPass.cs>) | M | 11 | 9 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/VirtualShadowMap/VirtualShadowMapCacheManager.cs](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/VirtualShadowMap/VirtualShadowMapCacheManager.cs>) | M | 4 | 2 |
| [com.jngame.tracy/Editor/CodeGen/JNTraceSourceStackILPostProcessor.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Editor/CodeGen/JNTraceSourceStackILPostProcessor.cs>) | A | 265 | 0 |
| [com.jngame.tracy/Editor/JNTracyManagedValidation.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Editor/JNTracyManagedValidation.cs>) | A | 359 | 0 |
| [com.jngame.tracy/Editor/JNTracyN11Validation.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Editor/JNTracyN11Validation.cs>) | A | 243 | 0 |
| [com.jngame.tracy/Editor/JNTracyN16SourceStackValidation.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Editor/JNTracyN16SourceStackValidation.cs>) | A | 119 | 0 |
| [com.jngame.tracy/Editor/JNTracyT3AutoLogin.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Editor/JNTracyT3AutoLogin.cs>) | A | 1575 | 0 |
| [com.jngame.tracy/Runtime/JNTraceFrameImage.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceFrameImage.cs>) | A | 965 | 0 |
| [com.jngame.tracy/Runtime/JNTraceGc.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceGc.cs>) | A | 205 | 0 |
| [com.jngame.tracy/Runtime/JNTraceGpuCatalog.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceGpuCatalog.cs>) | A | 351 | 0 |
| [com.jngame.tracy/Runtime/JNTraceGpuPassMarker.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceGpuPassMarker.cs>) | A | 169 | 0 |
| [com.jngame.tracy/Runtime/JNTraceGpuTaxonomy.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceGpuTaxonomy.cs>) | A | 42 | 0 |
| [com.jngame.tracy/Runtime/JNTraceIO.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceIO.cs>) | A | 132 | 0 |
| [com.jngame.tracy/Runtime/JNTraceLock.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceLock.cs>) | A | 80 | 0 |
| [com.jngame.tracy/Runtime/JNTraceLua.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceLua.cs>) | A | 125 | 0 |
| [com.jngame.tracy/Runtime/JNTraceMarker.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceMarker.cs>) | A | 289 | 0 |
| [com.jngame.tracy/Runtime/JNTraceMemory.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceMemory.cs>) | A | 48 | 0 |
| [com.jngame.tracy/Runtime/JNTracePlot.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTracePlot.cs>) | A | 18 | 0 |
| [com.jngame.tracy/Runtime/JNTraceScope.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceScope.cs>) | A | 34 | 0 |
| [com.jngame.tracy/Runtime/JNTraceScriptStack.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceScriptStack.cs>) | A | 247 | 0 |
| [com.jngame.tracy/Runtime/JNTraceSourceStack.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceSourceStack.cs>) | A | 189 | 0 |
| [com.jngame.tracy/Runtime/JNTraceUtf8.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceUtf8.cs>) | A | 20 | 0 |
| [com.jngame.tracy/Runtime/JNTraceUtility.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceUtility.cs>) | A | 119 | 0 |
| [com.jngame.tracy/Runtime/JNTracyNative.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTracyNative.cs>) | A | 579 | 0 |
| [com.jngame.tracy/Runtime/JNTracyRuntime.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTracyRuntime.cs>) | A | 877 | 0 |
| [com.jngame.tracy/Runtime/JNTracyT3PlayerAutoLogin.cs](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTracyT3PlayerAutoLogin.cs>) | A | 851 | 0 |
| [com.jngame.tracy/Tools/Invoke-JNTracyPlayerAutoCapture.ps1](<C:/workflow/PackageRepo/com.jngame.tracy/Tools/Invoke-JNTracyPlayerAutoCapture.ps1>) | A | 386 | 0 |
| [com.unity.render-pipelines.core/Runtime/RenderPipeline/ProfilingScope.cs](<C:/workflow/PackageRepo/com.unity.render-pipelines.core/Runtime/RenderPipeline/ProfilingScope.cs>) | M | 53 | 1 |

## 配置与数据合同

| 文件 | 变化 | 新增 | 删除 |
|---|---|---:|---:|
| [com.jngame.assets-pipeline/package.json](<C:/workflow/PackageRepo/com.jngame.assets-pipeline/package.json>) | M | 3 | 2 |
| [com.jngame.render-pipelines/package.json](<C:/workflow/PackageRepo/com.jngame.render-pipelines/package.json>) | M | 1 | 0 |
| [com.jngame.tracy/package.json](<C:/workflow/PackageRepo/com.jngame.tracy/package.json>) | A | 16 | 0 |

## Unity 元数据

| 文件 | 变化 | 新增 | 删除 |
|---|---|---:|---:|
| [com.jngame.render-pipelines/Editor/Diagnostics/JNTracyGpuValidation.cs.meta](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Editor/Diagnostics/JNTracyGpuValidation.cs.meta>) | A | 11 | 0 |
| [com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/JNVirtualGeometryTracyCatalog.cs.meta](<C:/workflow/PackageRepo/com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/JNVirtualGeometryTracyCatalog.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Editor.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Editor.meta>) | A | 8 | 0 |
| [com.jngame.tracy/Editor/CodeGen.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Editor/CodeGen.meta>) | A | 8 | 0 |
| [com.jngame.tracy/Editor/CodeGen/JNGame.Tracy.CodeGen.asmdef.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Editor/CodeGen/JNGame.Tracy.CodeGen.asmdef.meta>) | A | 7 | 0 |
| [com.jngame.tracy/Editor/CodeGen/JNTraceSourceStackILPostProcessor.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Editor/CodeGen/JNTraceSourceStackILPostProcessor.cs.meta>) | A | 2 | 0 |
| [com.jngame.tracy/Editor/JNGame.Tracy.Editor.asmdef.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Editor/JNGame.Tracy.Editor.asmdef.meta>) | A | 7 | 0 |
| [com.jngame.tracy/Editor/JNTracyManagedValidation.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Editor/JNTracyManagedValidation.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Editor/JNTracyN11Validation.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Editor/JNTracyN11Validation.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Editor/JNTracyN16SourceStackValidation.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Editor/JNTracyN16SourceStackValidation.cs.meta>) | A | 2 | 0 |
| [com.jngame.tracy/Editor/JNTracyT3AutoLogin.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Editor/JNTracyT3AutoLogin.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime.meta>) | A | 8 | 0 |
| [com.jngame.tracy/Runtime/JNGame.Tracy.asmdef.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNGame.Tracy.asmdef.meta>) | A | 7 | 0 |
| [com.jngame.tracy/Runtime/JNTraceFrameImage.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceFrameImage.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTraceGc.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceGc.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTraceGpuCatalog.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceGpuCatalog.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTraceGpuPassMarker.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceGpuPassMarker.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTraceGpuTaxonomy.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceGpuTaxonomy.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTraceIO.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceIO.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTraceLock.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceLock.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTraceLua.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceLua.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTraceMarker.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceMarker.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTraceMemory.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceMemory.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTracePlot.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTracePlot.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTraceScope.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceScope.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTraceScriptStack.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceScriptStack.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTraceSourceStack.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceSourceStack.cs.meta>) | A | 2 | 0 |
| [com.jngame.tracy/Runtime/JNTraceUtf8.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceUtf8.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTraceUtility.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTraceUtility.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTracyNative.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTracyNative.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTracyRuntime.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTracyRuntime.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Runtime/JNTracyT3PlayerAutoLogin.cs.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTracyT3PlayerAutoLogin.cs.meta>) | A | 11 | 0 |
| [com.jngame.tracy/Tools.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Tools.meta>) | A | 8 | 0 |
| [com.jngame.tracy/Tools/Invoke-JNTracyPlayerAutoCapture.ps1.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Tools/Invoke-JNTracyPlayerAutoCapture.ps1.meta>) | A | 7 | 0 |
| [com.jngame.tracy/Tools/Test-JNTracyPlayerAutomation.ps1.meta](<C:/workflow/PackageRepo/com.jngame.tracy/Tools/Test-JNTracyPlayerAutomation.ps1.meta>) | A | 7 | 0 |
| [com.jngame.tracy/package.json.meta](<C:/workflow/PackageRepo/com.jngame.tracy/package.json.meta>) | A | 7 | 0 |

## 测试与验收工具

| 文件 | 变化 | 新增 | 删除 |
|---|---|---:|---:|
| [com.jngame.tracy/Tools/Test-JNTracyPlayerAutomation.ps1](<C:/workflow/PackageRepo/com.jngame.tracy/Tools/Test-JNTracyPlayerAutomation.ps1>) | A | 101 | 0 |
