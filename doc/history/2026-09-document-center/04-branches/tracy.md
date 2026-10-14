> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# tracy：分支与工作区详细说明

> 只读快照：2026-09-07 19:55:48 +08:00；本地 refs，未 fetch；没有执行构建或验收。
> [返回分支地图](README.md) · [返回文档入口](../README.md) · [完整 Git 数据](../evidence/repository-snapshot.json)

仓库：C:/CodeProjects/GodotProjects/tracy-0.13.1。比较基线：dev。工作区状态仅统计 tracked 修改，未枚举 untracked；多仓观测不是原子快照。

## 工作区

| 路径 | 分支 | HEAD | tracked 修改数 |
|---|---|---|---:|
| C:/CodeProjects/GodotProjects/tracy-0.13.1 | dev | 9abafc1067d991d4373c54b912490e373f716283 | 0 |
| C:/CodeProjects/GodotProjects/tracy-0.13.1-ai-query-scan | feature/JNTracy-AI-QueryScan | fd4388533e1c6b31074dfa1c46aa3d73ff1ec591 | 24 |
| C:/CodeProjects/GodotProjects/tracy-0.13.1-dev-study | docs/tracy-code-understanding | 4ee5ad4673cae6f615283923636251e62dc490a1 | 3 |
| C:/CodeProjects/GodotProjects/tracy-0.13.1-n29-gpu-analysis | feature/JNTracy-N29-GpuResourceAnalysis | 15ceef123bed1ae0a87ed0cb4af908c3b19e83e8 | 0 |
| C:/CodeProjects/GodotProjects/tracy-0.13.1-n30-long-trace-session | feature/JNTracy-N30-LongTraceSessionStore | 8df7b0a19e08c93dfb9c8bc51aee721462489684 | 0 |
| C:/CodeProjects/GodotProjects/tracy-0.13.1-profiler-gui | feature/JNTracy-Profiler-GUI | f183d06f32b1cf256da35a1c56f46ed71e0012aa | 0 |
| C:/CodeProjects/GodotProjects/tracy-0.13.1-upstream-study | detached HEAD | 05cceee0df3b8d7c6fa87e9638af311dbabc63cb | 0 |
| C:/CodeProjects/GodotProjects/tracy-stream-convert-opt | feature/JNTracy-StreamConvert-Performance | 81ed7d35ad9fe9e35a6e0d74931cb0080b2acde5 | 0 |

## 比较口径

left/right 分别是“比较基线独有提交数 / 本分支独有提交数”。从 merge-base 到分支 tip 的文件变化仅用于定位领域，不等于相对当前工作区的完整 diff，也不证明内容已验收。right=0 只能说明本地提交图已被基线包含，不代表远端发布。无 merge-base 时文件差异不可用，不能记为0。

## archive/dev-N26-ResourceGraph

本地提交图确认：该分支tip已被比较基线包含；这不是远端发布或人工验收证明。

N26 ResourceGraph 工具链 checkpoint 归档。仅用于历史追溯；历史 section/schema 兼容需先核对，不用当前 GUI 直接假定可读。

- HEAD：6d6e9db011737352382b38ad8421da51d03e9db3
- 比较基线：dev；left/right：33	0
- merge-base：6d6e9db011737352382b38ad8421da51d03e9db3；比较状态：Available
- 跟踪引用：没有配置 upstream；不据此判断是否推送过

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 6d6e9db0 chore(jntracy): checkpoint N26 resource graph toolchain
- 721c3593 Fix GPU memory pointer generation attribution
- 89e0f742 Fix dense stream replay backpressure
- c54d592f N25.7 validate system trace in one pass
- 7d98444b N25.7 reuse stream snapshots and allocation callsites

从共同基点到分支 tip 没有独有文件变化；基线本身或已被基线包含的旧tip会出现此结果。


## dev

Tracy 团队集成基线，当前 tip 9abafc10；最近提交记录 protected branch publication flow。N29/N30/QueryScan 的专项能力不因共用仓库就自动存在于 dev。部署前使用当前产品合同与文件 hash。

- HEAD：9abafc1067d991d4373c54b912490e373f716283
- 比较基线：dev；left/right：0	0
- merge-base：9abafc1067d991d4373c54b912490e373f716283；比较状态：Available
- 跟踪引用：origin/dev 

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 9abafc10 Document protected branch publication flow
- 8ca65f88 Add JN Tracy AI diagnosis skill and team deployment guide
- f9956f3e Fix quadratic GPU Catalog stream conversion
- f183d06f N27-GUI-G9 finalize frame-scoped CPU memory evidence
- 33ffabe0 N27-GUI-G8 build and acceptance

从共同基点到分支 tip 没有独有文件变化；基线本身或已被基线包含的旧tip会出现此结果。


## docs/tracy-code-understanding

Tracy 源码理解文档分支，最近提交映射 server data model；独立 study worktree 有 tracked 修改。用于理解源码，不作为经过验收的产品工具分支。

- HEAD：4ee5ad4673cae6f615283923636251e62dc490a1
- 比较基线：dev；left/right：3	4
- merge-base：f183d06f32b1cf256da35a1c56f46ed71e0012aa；比较状态：Available
- 跟踪引用：没有配置 upstream；不据此判断是否推送过

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 4ee5ad46 docs: map Tracy server data model
- e395d0d1 docs: map Tracy client event pipeline
- 8ea98653 docs: map Tracy upstream architecture
- 22e5dd3d docs: establish Tracy code-understanding baseline
- f183d06f N27-GUI-G9 finalize frame-scoped CPU memory evidence

从共同基点到分支 tip 改动 4 个文件；主要目录及部分路径：

- doc：4 文件
- 路径示例：doc/learning/TRACY_CHECKPOINTS.zh-CN.md
- 路径示例：doc/learning/TRACY_CODE_UNDERSTANDING_PLAN.zh-CN.md
- 路径示例：doc/learning/TRACY_EVIDENCE_LEDGER.md
- 路径示例：doc/learning/TRACY_GLOSSARY.zh-CN.md

## feature/JNTracy-AI-QueryScan

Query 1.35 原生扫描专项，HEAD 对应 QSCAN13 阶段验收，工作区另有 QSCAN14 tracked 修改。负责中立聚合、候选策略、异步 scan、统计和报告合同；配套草案 Skill 2.0.1，安装版 1.1.3 不能视作已经同步。FW5 内存保护失败仍未闭环。

- HEAD：fd4388533e1c6b31074dfa1c46aa3d73ff1ec591
- 比较基线：dev；left/right：2	89
- merge-base：f9956f3ec941d41062e04cd0d57eafba64b4c2e7；比较状态：Available
- 跟踪引用：没有配置 upstream；不据此判断是否推送过

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- fd438853 QSCAN-13 validate deterministic scan on real traces
- 2ea94e86 QSCAN-10 Add persistent asynchronous scan manager and MCP API
- 4e1352a2 QSCAN-9 Add deterministic candidate policy evaluation
- f3be0971 QSCAN-8 Add exact statistics anomaly detection and neutral audit
- 07d4d10d QSCAN-7 Add memory IO sampling and telemetry scanners

从共同基点到分支 tip 改动 156 个文件；主要目录及部分路径：

- analysis：84 文件
- query：45 文件
- doc：8 文件
- profiler：5 文件
- capture：4 文件
- server：4 文件
- 路径示例：analysis/TracyAnalysis.cpp
- 路径示例：analysis/TracyAnalysisProfile.cpp
- 路径示例：analysis/TracyAnalysisProfile.hpp
- 路径示例：analysis/TracyBoundedScanCursor.cpp
- 路径示例：analysis/TracyBoundedScanCursor.hpp
- 路径示例：analysis/TracyCandidatePolicy.cpp
- 路径示例：analysis/TracyCandidatePolicy.hpp
- 路径示例：analysis/TracyDeterministicScanTypes.hpp

## feature/JNTracy-N28-ResourceGraph

ResourceGraph Schema3 工具端，配套引擎/Package N28 分支。不能把其 schema 与 N27 section12/Catalog1 任意混配；本轮未核定最终生产验收。

- HEAD：95325623957d2cbffa142b09716c58d6053a3766
- 比较基线：dev；left/right：33	1
- merge-base：6d6e9db011737352382b38ad8421da51d03e9db3；比较状态：Available
- 跟踪引用：没有配置 upstream；不据此判断是否推送过

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 95325623 feat(jntracy): support resource graph schema 3
- 6d6e9db0 chore(jntracy): checkpoint N26 resource graph toolchain
- 721c3593 Fix GPU memory pointer generation attribution
- 89e0f742 Fix dense stream replay backpressure
- c54d592f N25.7 validate system trace in one pass

从共同基点到分支 tip 改动 28 个文件；主要目录及部分路径：

- query：18 文件
- public：4 文件
- analysis：3 文件
- server：3 文件
- 路径示例：analysis/TracyTraceSource.hpp
- 路径示例：analysis/TracyWorkerTraceSource.cpp
- 路径示例：analysis/TracyWorkerTraceSource.hpp
- 路径示例：public/client/TracyJnClient.hpp
- 路径示例：public/client/TracyProfiler.cpp
- 路径示例：public/common/TracyProtocol.hpp
- 路径示例：public/common/TracyQueue.hpp
- 路径示例：query/schema/coverage-fields-v1.json

## feature/JNTracy-N29-GpuResourceAnalysis

N29 GPU Resource Analysis sidecar 实现。Converter、外部 Builder、Query/Profiler 共用持久 reader；历史验收 Passed_With_Declared_Exceptions，真实60分钟/最终GUI交互和Builder优先级有声明边界。N30 基于该分支 tip 开始。

- HEAD：15ceef123bed1ae0a87ed0cb4af908c3b19e83e8
- 比较基线：dev；left/right：2	1
- merge-base：f9956f3ec941d41062e04cd0d57eafba64b4c2e7；比较状态：Available
- 跟踪引用：没有配置 upstream；不据此判断是否推送过

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 15ceef12 Implement N29 GPU resource analysis sidecar
- f9956f3e Fix quadratic GPU Catalog stream conversion
- f183d06f N27-GUI-G9 finalize frame-scoped CPU memory evidence
- 33ffabe0 N27-GUI-G8 build and acceptance
- 739b4917 N27-GUI-G7 enhance jobs source provenance and navigation

从共同基点到分支 tip 改动 40 个文件；主要目录及部分路径：

- query：15 文件
- analysis：14 文件
- profiler：5 文件
- capture：2 文件
- doc：2 文件
- cmake：1 文件
- 路径示例：analysis/TracyGpuAnalysis.cpp
- 路径示例：analysis/TracyGpuAnalysis.hpp
- 路径示例：analysis/TracyGpuAnalysisCache.cpp
- 路径示例：analysis/TracyGpuAnalysisPath.hpp
- 路径示例：analysis/TracyGpuAnalysisSidecar.cpp
- 路径示例：analysis/TracyGpuAnalysisSidecar.hpp
- 路径示例：analysis/TracyGpuAnalysisStore.cpp
- 路径示例：analysis/TracyGpuAnalysisStore.hpp

## feature/JNTracy-N30-LongTraceSessionStore

N30/N30A 长 Trace Canonical Session、Mandatory Derived 和 Final Audit。历史 A7 已发布一次源质量降级的30分钟Session；2026-09-07 新 P3 双路径计数与响应门禁失败，后续新30分钟验收停止。当前 tip 补记失败边界；不可标记全部通过。

- HEAD：8df7b0a19e08c93dfb9c8bc51aee721462489684
- 比较基线：dev；left/right：2	82
- merge-base：f9956f3ec941d41062e04cd0d57eafba64b4c2e7；比较状态：Available
- 跟踪引用：origin/feature/JNTracy-N30-LongTraceSessionStore [ahead 6]

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 8df7b0a1 N30A.5 Record short differential failures and explicit evidence boundaries
- e3bfec71 N30A.4 Isolate short replay input and stop on prerequisite failure
- 33f29eba N30A.3 Use offline full Worker for short differential oracle
- 9c74cd3e N30A.2 Add deterministic query anchors and explicit comparison diagnostics
- 5846135f N30A.1 Add deterministic full acceptance harness and evidence logs

从共同基点到分支 tip 改动 188 个文件；主要目录及部分路径：

- tools：67 文件
- analysis：66 文件
- query：28 文件
- doc：8 文件
- profiler：5 文件
- capture：4 文件
- 路径示例：.gitignore
- 路径示例：analysis/TracyAnalysis.cpp
- 路径示例：analysis/TracyGpuAnalysis.cpp
- 路径示例：analysis/TracyGpuAnalysis.hpp
- 路径示例：analysis/TracyGpuAnalysisCache.cpp
- 路径示例：analysis/TracyGpuAnalysisPath.hpp
- 路径示例：analysis/TracyGpuAnalysisSidecar.cpp
- 路径示例：analysis/TracyGpuAnalysisSidecar.hpp

## feature/JNTracy-Profiler-GUI

本地提交图确认：该分支tip已被比较基线包含；这不是远端发布或人工验收证明。

N27 GUI 独立预览分支，GPU Catalog/物理显存/Pass 查询与 CPU Memory 视图。集中报告为 PreviewCandidate，要求人工体验接受；最新提交另有 G9 frame-scoped CPU memory，不能只据旧 G0-G8 报告认证整个当前tip。

- HEAD：f183d06f32b1cf256da35a1c56f46ed71e0012aa
- 比较基线：dev；left/right：3	0
- merge-base：f183d06f32b1cf256da35a1c56f46ed71e0012aa；比较状态：Available
- 跟踪引用：没有配置 upstream；不据此判断是否推送过

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- f183d06f N27-GUI-G9 finalize frame-scoped CPU memory evidence
- 33ffabe0 N27-GUI-G8 build and acceptance
- 739b4917 N27-GUI-G7 enhance jobs source provenance and navigation
- b2f50b3b N27-GUI-G6 rebuild CPU memory workflow
- c4d7188c N27-GUI-G5 add GPU evidence analysis pages

从共同基点到分支 tip 没有独有文件变化；基线本身或已被基线包含的旧tip会出现此结果。


## feature/JNTracy-StreamConvert-Performance

离线转换优化及语义 oracle 分支。阶段优化解决 reference集合重复搬迁，并保留顺序语义；最新提交加强 Job semantic hash oracle。历史速度数据不是对当前所有协议/输入的普遍承诺。

- HEAD：81ed7d35ad9fe9e35a6e0d74931cb0080b2acde5
- 比较基线：dev；left/right：34	8
- merge-base：721c3593ac0cd31aa0def367ab246b6b81afcd79；比较状态：Available
- 跟踪引用：没有配置 upstream；不据此判断是否推送过

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- 81ed7d35 test(stream-convert): add complete job semantic hash oracle
- 0f3581b7 SC.6 finalize offline converter release candidate
- b8bbadab SC.6 fix offline protocol drain resource definitions
- 97c084d7 SC.5 add MCP domain semantic comparison
- 3ddce2ef SC.2-SC.4 add bounded offline conversion pipeline

从共同基点到分支 tip 改动 39 个文件；主要目录及部分路径：

- query：21 文件
- capture：4 文件
- analysis：3 文件
- doc：3 文件
- public：3 文件
- server：3 文件
- 路径示例：analysis/TracyTraceSource.hpp
- 路径示例：analysis/TracyWorkerTraceSource.cpp
- 路径示例：analysis/TracyWorkerTraceSource.hpp
- 路径示例：capture/CMakeLists.txt
- 路径示例：capture/src/stream-convert.cpp
- 路径示例：capture/src/stream-inspect.cpp
- 路径示例：capture/src/trace-compare.cpp
- 路径示例：doc/JN-Tracy-Stream-Convert-Offline-Optimization-Acceptance.zh-CN.md

## fix/gpu-catalog-offline-convert-quadratic

本地提交图确认：该分支tip已被比较基线包含；这不是远端发布或人工验收证明。

GPU Catalog 离线转换近二次复杂度修复分支，其 f9956f3e 是 N29 的历史基线，也是旧测试机快照的一部分。与更完整的 StreamConvert-Performance 分支分开说明。

- HEAD：f9956f3ec941d41062e04cd0d57eafba64b4c2e7
- 比较基线：dev；left/right：2	0
- merge-base：f9956f3ec941d41062e04cd0d57eafba64b4c2e7；比较状态：Available
- 跟踪引用：origin/fix/gpu-catalog-offline-convert-quadratic 

最近提交（可能包含继承历史，不代表每条均为该分支独有）：

- f9956f3e Fix quadratic GPU Catalog stream conversion
- f183d06f N27-GUI-G9 finalize frame-scoped CPU memory evidence
- 33ffabe0 N27-GUI-G8 build and acceptance
- 739b4917 N27-GUI-G7 enhance jobs source provenance and navigation
- b2f50b3b N27-GUI-G6 rebuild CPU memory workflow

从共同基点到分支 tip 没有独有文件变化；基线本身或已被基线包含的旧tip会出现此结果。


## 开发前复核

重新检查目标 worktree、HEAD、tracked/untracked、工具 EXE SHA 和配套仓库；保留其他任务改动。分支注释是资料综合说明，不是本次代码审计或批准合并；每个分支的历史限制见迭代总览和来源索引。

