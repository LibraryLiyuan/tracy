> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# jnunity：分支与工作区详细说明

> 只读快照：2026-09-07 19:55:48 +08:00；本地 refs，未 fetch；没有执行构建或验收。
> [返回分支地图](README.md) · [返回文档入口](../README.md) · [完整 Git 数据](../evidence/repository-snapshot.json)

仓库：C:/workflow/jnunity。比较基线：feature/JNTracy。工作区状态仅统计 tracked 修改，未枚举 untracked；多仓观测不是原子快照。

## 工作区

| 路径 | 分支 | HEAD | tracked 修改数 |
|---|---|---|---:|
| C:/workflow/jnunity | feature/JNTracy-DXR-GpuTiming-20260907 | 0f3f7d6f74d7f08f69605fcfcd374732b2322758 | 0 |

## 比较口径

left/right 分别是“比较基线独有提交数 / 本分支独有提交数”。从 merge-base 到分支 tip 的文件变化仅用于定位领域，不等于相对当前工作区的完整 diff，也不证明内容已验收。right=0 只能说明本地提交图已被基线包含，不代表远端发布。无 merge-base 时文件差异不可用，不能记为0。

## archive/JNTracy-Legacy

历史 JNTracy 采集 profile 指南归档，提交摘要指向 2022.3.67 迁移前记录。本地无法与 feature/JNTracy 求得 merge-base；不推定可以直接合并，也不据此判定远端历史不存在。

- HEAD：2f24454d593b7a1705049f357a9cf68452187a98
- 比较基线：feature/JNTracy；left/right：1185	861
- merge-base：；比较状态：No merge base available locally; diff unavailable
- 跟踪引用：没有配置 upstream；不据此判断是否推送过

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 2f24454d59 docs(tracy): archive capture profile guidance before 2022.3.67 migration
- 7c958cfcca JN Tracy N17 complete full acceptance and performance matrix
- 4db805212a JN Tracy N17 complete Query MCP pressure acceptance
- e8eb22d4e6 JN Tracy N17 complete startup I/O memory acceptance
- a1f8035b4f JN Tracy N17 complete FrameImage acceptance

## archive/JNTracy-N26-ResourceGraph-2022.3.67f1

本地提交图确认：该分支tip已被比较基线包含；这不是远端发布或人工验收证明。

迁移与 N26 ResourceGraph 计划的归档点。用于追溯旧方案和回退背景；当前版本配套以 N27/N28 专题核对，归档名不构成生产可用证明。

- HEAD：fb2cb750348e76ece512aa7c08c82d3f853c0064
- 比较基线：feature/JNTracy；left/right：33	0
- merge-base：fb2cb750348e76ece512aa7c08c82d3f853c0064；比较状态：Available
- 跟踪引用：没有配置 upstream；不据此判断是否推送过

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- fb2cb75034 docs(jntracy): archive migration and resource graph plans
- 3a95a660c7 Merge origin/T3/unity-2022.3.67f1 into feature/JNTracy-2022.3.67f1
- 2a1d4306e1 feat(tracy): add N26 Unity resource evidence graph
- ebcb5a2401 #806927  optimize DXR SBT
- da7c437377 docs: update agent workflow guidance

从共同基点到分支 tip 没有独有文件变化；基线本身或已被基线包含的旧tip会出现此结果。


## debug/N27-catalog-generation-close-20260827

本地提交图确认：该分支tip已被比较基线包含；这不是远端发布或人工验收证明。

GPU Catalog generation close 修复与文档分支。该 HEAD 也是旧同事部署快照中的引擎版本；处理连接代际/收尾问题时先读对应修复报告，不能把旧部署 hash 当当前引擎版本。

- HEAD：8df2ef2b1e658f4c3d7c0c155f06398b80c64c18
- 比较基线：feature/JNTracy；left/right：9	0
- merge-base：8df2ef2b1e658f4c3d7c0c155f06398b80c64c18；比较状态：Available
- 跟踪引用：没有配置 upstream；不据此判断是否推送过

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 8df2ef2b1e Document GPU Catalog generation close fix
- 973868fc5c Fix GPU Catalog page ownership ABA
- bd6dd182e5 Merge origin/T3/unity-2022.3.67f1 into feature/JNTracy
- 85420efd01 fix(jntracy): preserve GPU marker provenance
- 9423b4d14a #805044 sync transparent jobs before cull result cleanup

从共同基点到分支 tip 没有独有文件变化；基线本身或已被基线包含的旧tip会出现此结果。


## feature/JNTracy

JNTracy 引擎集成基线。当前 tip 为保留 Unity marker 的 Render Thread zone nesting 修复。相对本地 origin 跟踪引用 ahead 状态见下方；尚未 fetch，不能声称远端实时状态。

- HEAD：4392d3acb65c0232ded945eb5603e53d56e7b09d
- 比较基线：feature/JNTracy；left/right：0	0
- merge-base：4392d3acb65c0232ded945eb5603e53d56e7b09d；比较状态：Available
- 跟踪引用：origin/feature/JNTracy [ahead 9]

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 4392d3acb6 Fix Render thread Tracy zone nesting without dropping markers
- 9c9aa3f790 Merge remote-tracking branch 'origin/T3/unity-2022.3.67f1' into feature/JNTracy
- 7b94f9c310 #806927 DXR: guard asynchronous external CBV publication
- 6e368d0662 feat: add GPU skinning batching
- 8df2ef2b1e Document GPU Catalog generation close fix

从共同基点到分支 tip 没有独有文件变化；基线本身或已被基线包含的旧tip会出现此结果。


## feature/JNTracy-DXR-GpuTiming-20260907

当前检出的 DXR timing-only 专项分支，从 4392d3ac 基线增加 BLAS batch、TLAS build、DispatchRays GPU 时间。晚于原交付报告的 helper guard 提交已在 HEAD 中；原成功采集与后续 Player device removed 分别保留，未在本次验证新 HEAD 二进制。

- HEAD：0f3f7d6f74d7f08f69605fcfcd374732b2322758
- 比较基线：feature/JNTracy；left/right：0	2
- merge-base：4392d3acb65c0232ded945eb5603e53d56e7b09d；比较状态：Available
- 跟踪引用：没有配置 upstream；不据此判断是否推送过

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 0f3f7d6f74 Guard Tracy resource reference helper definition with ENABLE_JN_TRACY
- 18a46a79c6 Add timing-only native DXR GPU zones with capture regression tests
- 4392d3acb6 Fix Render thread Tracy zone nesting without dropping markers
- 9c9aa3f790 Merge remote-tracking branch 'origin/T3/unity-2022.3.67f1' into feature/JNTracy
- 7b94f9c310 #806927 DXR: guard asynchronous external CBV publication

从共同基点到分支 tip 改动 8 个文件；主要目录及部分路径：

- Runtime：8 文件
- 路径示例：Runtime/GfxDevice/d3d12/D3D12CommandList.cpp
- 路径示例：Runtime/GfxDevice/d3d12/D3D12CommandList.h
- 路径示例：Runtime/GfxDevice/d3d12/DXR/GfxDeviceDXR.cpp
- 路径示例：Runtime/GfxDevice/d3d12/JNTracyD3D12Bridge.cpp
- 路径示例：Runtime/GfxDevice/d3d12/JNTracyD3D12Bridge.h
- 路径示例：Runtime/Profiler/JNTracy/Docs/DXR-GPU-Timing-20260907.zh-CN.md
- 路径示例：Runtime/Profiler/JNTracy/Tests/JNTracyDxrGpuTimingIntegrationTests.py
- 路径示例：Runtime/Profiler/JNTracy/Tests/JNTracyDxrPlayerLoadTests.py

## feature/JNTracy-N28-ResourceGraph

N28 ResourceGraph Schema3 引擎实现分支，需要配套 PackageRepo 与 Tracy 的 N28 分支理解。目标为 Asset/Object/资源/CPU-GPU Memory 双向证据链；本轮未核定最终真实验收，不与 N27 Catalog 功能混称。

- HEAD：5111e048476638d3c297f4c2ca3d541c0c0c2168
- 比较基线：feature/JNTracy；left/right：33	1
- merge-base：fb2cb750348e76ece512aa7c08c82d3f853c0064；比较状态：Available
- 跟踪引用：没有配置 upstream；不据此判断是否推送过

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 5111e04847 feat(jntracy): implement N28 resource graph schema 3
- fb2cb75034 docs(jntracy): archive migration and resource graph plans
- 3a95a660c7 Merge origin/T3/unity-2022.3.67f1 into feature/JNTracy-2022.3.67f1
- 2a1d4306e1 feat(tracy): add N26 Unity resource evidence graph
- ebcb5a2401 #806927  optimize DXR SBT

从共同基点到分支 tip 改动 29 个文件；主要目录及部分路径：

- Runtime：25 文件
- External：4 文件
- 路径示例：External/Tracy/public/client/TracyJnClient.hpp
- 路径示例：External/Tracy/public/client/TracyProfiler.cpp
- 路径示例：External/Tracy/public/common/TracyProtocol.hpp
- 路径示例：External/Tracy/public/common/TracyQueue.hpp
- 路径示例：Runtime/GfxDevice/d3d12/DXR/GfxDeviceDXR.cpp
- 路径示例：Runtime/Profiler/JNTracy/Config/JNTracy.Daily.ini
- 路径示例：Runtime/Profiler/JNTracy/Config/JNTracy.HighEvidence.ini
- 路径示例：Runtime/Profiler/JNTracy/Config/JNTracy.ini

## feature/PIX-performance-analysis

PIX 性能分析相关旁支，最近提交是合并冲突修复。当前任务未审计其 Tracy 能力与配套工具；本地与 JNTracy 基线无可用 merge-base，差异栏不按零变更处理。

- HEAD：f11bb0007779cc3a4fb3821bb50bb9aa4907c1e1
- 比较基线：feature/JNTracy；left/right：1185	797
- merge-base：；比较状态：No merge base available locally; diff unavailable
- 跟踪引用：origin/feature/PIX-performance-analysis 

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- f11bb00077 fix: 修复合并冲突的existingbuffer报错
- 186e750ed5 Merge branch 'mainline-2022' into feature/PIX-performance-analysis
- 29cc32978a fix(pix): support non-development player builds
- 269c5e0f37 DX12 PSO collection
- 4f0f1a399b fix null cbv on versioned buffer and gfx jobs, fix descropter double free, save release raytracing data

## mainline-2022

引擎 mainline-2022 基线分支，保留普通引擎演进。不是本次已核验的 JNTracy 运行入口；本地 merge-base 不可用，历史关系待有明确需求再查。

- HEAD：47e0a6c1866b2f9628584feea00e31b7c5c132a0
- 比较基线：feature/JNTracy；left/right：1185	800
- merge-base：；比较状态：No merge base available locally; diff unavailable
- 跟踪引用：origin/mainline-2022 

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 47e0a6c186 add scratch buffer map fail fast, fix slot jugle logic bug, add died rt protect
- 3424109f5c #812738 change job worker count to physical processor from logical processor
- bdcd014a62 fix: prevent SVT tile extraction crashes
- c2abedcd4b #810850 RenderDoc support ray tracing
- 92a6e56292 clean pending instance before remove

## 开发前复核

重新检查目标 worktree、HEAD、tracked/untracked、工具 EXE SHA 和配套仓库；保留其他任务改动。分支注释是资料综合说明，不是本次代码审计或批准合并；每个分支的历史限制见迭代总览和来源索引。

