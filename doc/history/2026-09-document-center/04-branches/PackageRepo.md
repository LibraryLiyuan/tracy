> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# PackageRepo：分支与工作区详细说明

> 只读快照：2026-09-07 19:55:48 +08:00；本地 refs，未 fetch；没有执行构建或验收。
> [返回分支地图](README.md) · [返回文档入口](../README.md) · [完整 Git 数据](../evidence/repository-snapshot.json)

仓库：C:/workflow/PackageRepo。比较基线：feature/Pack-JNTracy。工作区状态仅统计 tracked 修改，未枚举 untracked；多仓观测不是原子快照。

## 工作区

| 路径 | 分支 | HEAD | tracked 修改数 |
|---|---|---|---:|
| C:/workflow/PackageRepo | feature/Pack-JNTracy | 8ea5f0bce9e06e0199d8a13183d06925b3c6d830 | 0 |
| C:/Users/Admin/Documents/JN-Unity-T3/worktrees/PackageRepo-E-ManagedOff-20260903 | detached HEAD | 5813ea3a321af01cca51d26b1b9379dc194a70ac | 12 |

## 比较口径

left/right 分别是“比较基线独有提交数 / 本分支独有提交数”。从 merge-base 到分支 tip 的文件变化仅用于定位领域，不等于相对当前工作区的完整 diff，也不证明内容已验收。right=0 只能说明本地提交图已被基线包含，不代表远端发布。无 merge-base 时文件差异不可用，不能记为0。

## archive/Pack-JNTracy-N26-ResourceGraph

本地提交图确认：该分支tip已被比较基线包含；这不是远端发布或人工验收证明。

N26 ResourceGraph 的 Package 归档，用于复现旧资源来源设计。不要与当前 N27 Catalog 或 N28 Schema3 混配。

- HEAD：8d24229ee74354517f6c44e21e5eb367ec15a34b
- 比较基线：feature/Pack-JNTracy；left/right：238	0
- merge-base：8d24229ee74354517f6c44e21e5eb367ec15a34b；比较状态：Available
- 跟踪引用：没有配置 upstream；不据此判断是否推送过

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 8d24229ee Merge origin/mainline into feature/Pack-JNTracy
- cd73af038 feat(tracy): connect Unity resource evidence graph
- 98e57a72b Merge 'feature/mesh-decal-pbr' into 'mainline'
- 6b8aeb8a5 Merge 'feature/fix-vg-transform-issue' into 'mainline'
- 203f4001a #824339 元素地面开发-引擎需求-mesh贴花

从共同基点到分支 tip 没有独有文件变化；基线本身或已被基线包含的旧tip会出现此结果。


## feature/Pack-JNTracy

当前 Package 集成分支；最近提交加入 managed Tracy compile-time switches。为 Managed、GPU 语义与资源桥提供配套代码。关闭编译开关只是隔离/部署选择，不证明 N31 Catalog 开启路径性能已达标。

- HEAD：8ea5f0bce9e06e0199d8a13183d06925b3c6d830
- 比较基线：feature/Pack-JNTracy；left/right：0	0
- merge-base：8ea5f0bce9e06e0199d8a13183d06925b3c6d830；比较状态：Available
- 跟踪引用：origin/feature/Pack-JNTracy 

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 8ea5f0bce Add compile-time switches for managed Tracy instrumentation
- 3c351164f Merge remote-tracking branch 'origin/t3/development' into feature/Pack-JNTracy
- 5813ea3a3 Merge 'mainline' into 't3/development'
- 5ac5a9507 Merge 'codex/raytracing-skinned-lod-version-cache' into 'mainline'
- c18c8f7df 优化自动光追 Skinned LOD 版本缓存

从共同基点到分支 tip 没有独有文件变化；基线本身或已被基线包含的旧tip会出现此结果。


## feature/Pack-JNTracy-N28-ResourceGraph

N28 managed resource graph provider 专项分支，需配套引擎和 Tracy Schema3。记录 Object/Asset/资源来源关系的计划与实现入口，当前生产验收未在本轮核定。

- HEAD：d23ff7d78ad44605ac6e1a3aa5ca9b4eb56e51d3
- 比较基线：feature/Pack-JNTracy；left/right：238	1
- merge-base：8d24229ee74354517f6c44e21e5eb367ec15a34b；比较状态：Available
- 跟踪引用：没有配置 upstream；不据此判断是否推送过

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- d23ff7d78 feat(jntracy): add N28 managed resource graph provider
- 8d24229ee Merge origin/mainline into feature/Pack-JNTracy
- cd73af038 feat(tracy): connect Unity resource evidence graph
- 98e57a72b Merge 'feature/mesh-decal-pbr' into 'mainline'
- 6b8aeb8a5 Merge 'feature/fix-vg-transform-issue' into 'mainline'

从共同基点到分支 tip 改动 12 个文件；主要目录及部分路径：

- com.jngame.assets-pipeline：8 文件
- com.jngame.tracy：4 文件
- 路径示例：com.jngame.assets-pipeline/Runtime/ResourceSystem/AssetsPipelineResourceGraphSource.cs
- 路径示例：com.jngame.assets-pipeline/Runtime/ResourceSystem/AssetsPipelineResourceGraphSource.cs.meta
- 路径示例：com.jngame.assets-pipeline/Runtime/ResourceSystem/ResourceSystem.Properties.cs
- 路径示例：com.jngame.assets-pipeline/Runtime/ResourceSystem/ResourceSystem.UnloadQueue.cs
- 路径示例：com.jngame.assets-pipeline/Runtime/ResourceSystem/ResourceSystem.Utilities.cs
- 路径示例：com.jngame.assets-pipeline/Runtime/ResourceSystem/ResourceSystem.cs
- 路径示例：com.jngame.assets-pipeline/Runtime/ResourceSystem/Structs/AssetObject.cs
- 路径示例：com.jngame.assets-pipeline/Runtime/ResourceSystem/Structs/SubAssetObject.cs

## feature/Pack-PIX-performance-analysis

PIX 分析相关 Package 旁支。只读 Git 快照确认存在；本轮不把它当 JNTracy 默认分支，也未审查全部功能差异。

- HEAD：163fcb6d14c4dad58c671ab1eff79844e377f046
- 比较基线：feature/Pack-JNTracy；left/right：1154	3
- merge-base：d1fadb7aa46ff607505ebdf0207f71da756a6ea4；比较状态：Available
- 跟踪引用：origin/feature/Pack-PIX-performance-analysis 

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 163fcb6d1 Merge branch 'mainline' into feature/Pack-PIX-performance-analysis
- d1fadb7aa Merge 'bug/fix-criware-shader-error' into 'mainline'
- bc0aa2ef7 #806724 【Shader编译错误】【SofdecPrimeRgb】打包时报 invalid semantic 'COLOR' for ps 6.0
- 83671a666 Merge branch 'mainline' into feature/Pack-PIX-performance-analysis
- 872646fee Merge 'feature/TMP-rectmask-softness' into 'mainline'

从共同基点到分支 tip 改动 13 个文件；主要目录及部分路径：

- com.jngame.render-pipelines：12 文件
- com.jngame.graphics-profiler：1 文件
- 路径示例：com.jngame.graphics-profiler/JNGpuPassStat/Runtime/JNGpuPassStatUtils.cs
- 路径示例：com.jngame.render-pipelines/Runtime/BuiltIn/RenderPasses/AdditionalFocusLightsShadowPass.cs
- 路径示例：com.jngame.render-pipelines/Runtime/BuiltIn/RenderPasses/AdditionalLightsShadowPass.cs
- 路径示例：com.jngame.render-pipelines/Runtime/BuiltIn/RenderPasses/ScreenProbePass.cs
- 路径示例：com.jngame.render-pipelines/Runtime/BuiltIn/RenderPipeline/JNRPGpuPassRegistry.cs
- 路径示例：com.jngame.render-pipelines/Runtime/BuiltIn/RenderUtils/ShadowUtils.cs
- 路径示例：com.jngame.render-pipelines/Runtime/BuiltIn/Shadow/MainLightShadowMapPass.cs
- 路径示例：com.jngame.render-pipelines/Runtime/BuiltIn/VirtualGeometry/Render/VGConsoleHWRasterPass.cs

## mainline

本地提交图确认：该分支tip已被比较基线包含；这不是远端发布或人工验收证明。

Package 上游主线，本地跟踪引用落后较多。behind 是本地缓存 origin 的比较结果，不触发自动更新或说明当前 JNTracy 能力。

- HEAD：afe6b86b5c6dd4b5cbf4c0b513ea948509de8d5e
- 比较基线：feature/Pack-JNTracy；left/right：939	0
- merge-base：afe6b86b5c6dd4b5cbf4c0b513ea948509de8d5e；比较状态：Available
- 跟踪引用：origin/mainline [behind 988]

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- afe6b86b5 Merge 'audio-dev-engine-memory-grab' into 'mainline'
- 5e02438ab #756481 【T3】【PR2.5版本】Wwise引擎支持Memory数据穿透和上报
- ab3c51dd5 Merge 'feature/restir12' into 'mainline'
- d9e95b675 region fog buffer null
- 5d5d9051f Merge 'feature/Glass_SSR' into 'mainline'

从共同基点到分支 tip 没有独有文件变化；基线本身或已被基线包含的旧tip会出现此结果。


## 开发前复核

重新检查目标 worktree、HEAD、tracked/untracked、工具 EXE SHA 和配套仓库；保留其他任务改动。分支注释是资料综合说明，不是本次代码审计或批准合并；每个分支的历史限制见迭代总览和来源索引。

