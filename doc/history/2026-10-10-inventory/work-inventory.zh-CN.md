> 历史归档：2026 年 10 月 10 日交接整理。当前版本与操作入口见 [文档首页](../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# Tracy AI 性能分析工作梳理与交接讨论稿

整理日期：2026 年 10 月 10 日。交接对象：接手开发维护的同事，同时兼顾工具使用者。

这项工作已经形成从 Unity 性能数据采集、持久化、查询，到 AI 调查、报告和人工回查的多仓工具链。交接需要同时说明系统如何工作、当前能使用什么、哪些结论有验证依据，以及哪些开发仍需继续。

现有资料较完整，但状态分散在不同阶段。最需要先解决的是版本和适用范围：文档中心主要停在 9 月 7 日；10 月 9 日的 dev 已有新的 EventStore、显存分类和 stream 访问能力；Skill、旧验收和配置说明尚未全部跟上。

本轮定位并读取了 113 条相关或相邻聊天的关键轮次，共 288 轮，并对照现有文档、部署记录、当前源码配置和本地 Git 状态。这里的“读取”不表示逐条审计了每个聊天的全部历史。本文保留历史结果的版本边界；产品测试结果来自对应验收记录，本轮核对了工具文件哈希，没有重新运行性能验收。完整入口见 [对话与资料索引](sources.zh-CN.md)。

## 一 整体工作范围

```mermaid
flowchart LR
    A[Unity 引擎与 Package 插桩] --> B[JNTracyClient]
    B --> C[Capture 与 stream 落盘]
    C --> D[转换与数据访问层]
    D --> E[Query 与 MCP]
    E --> F[确定性扫描与候选]
    F --> G[AI 证据调查与源码分析]
    G --> H[报告与便携交付]
    D --> I[原生与网页 Profiler]
    H --> I
```

Session Store、固定切片和新 EventStore 是不同阶段、不同成熟度的存储路线，具体关系见第五项。图中的数据访问层不表示它们已经全部合并或等价。

| 工作主线 | 已积累的内容 | 接手者需要掌握的重点 |
|---|---|---|
| 1 选型与总体架构 | Godot 原型、D3D12 探索、Unity 接入设计、结构化查询与 AI 工作流 | 为什么扩展 Tracy，各模块的责任边界 |
| 2 Unity 采集与执行关系 | CPU、Frame、Main/Render、C#/Lua、Job、栈、I/O、Sampling、Context Switch | 事件来自哪里，身份和层级如何传递，数据缺失如何表达 |
| 3 GPU 与显存归因 | Queue/Segment/Pass、DXR、ResourceGraph、GPU Catalog、显存和资源引用 | GPU 计时与 CPU 提交、逻辑资源与物理分配、引用与所有权的区别 |
| 4 采集配置与自动化 | INI 档位、自动登录、业务就绪、稳定帧、Capture GUI、正常停止 | 目标进程实际使用的配置、采集场景和停止方式 |
| 5 存储转换与长录制 | Journal、离线转换、JNTRACYX、磁盘索引、N29/N30、新 EventStore、固定切片 | 输入输出格式、兼容工具、资源成本、恢复与边界语义 |
| 6 Query 与 MCP | 结构化查询、全量扫描、Profile、Neutral/Policy 缓存、分页和恢复 | 数值计算的权威来源、查询完整性、能力与限制 |
| 7 AI Skill 与分析质量 | 1.x 至 2.3.1、候选分流、源码调查、证据审计、报告规则 | 何时能得出结论，何时需要补证，如何避免浅层分析 |
| 8 Profiler 与报告交付 | 原生 GUI、网页查看器、定位记录、截图、分册与便携包 | 报告如何回到同一个帧、线程和事件，展示缺口如何保留 |
| 9 构建部署与版本 | 三仓分支、Engine/IL2CPP 成套替换、符号、独立 Player、测试机部署 | 代码与资源契约、源码与二进制身份、安装和回滚 |
| 10 验证与知识维护 | Unity Profiler 对账、语义差分、开销实验、验收记录、文档中心 | 验证范围、已推翻结论、案例证据和后续维护入口 |

## 二 当前可复核基线

以下为 10 月 10 日读取的本地状态，未 fetch 远端；只统计受 Git 跟踪的文件变化，不能据此断言没有未跟踪文件。

| 仓库或工作区 | 当前分支 | HEAD | 受跟踪文件变化 |
|---|---|---|---:|
| `C:/workflow/jnunity` | `feature/JNTracyUnity` | `214a769e64ab` | 0 |
| `C:/workflow/PackageRepo` | `feature/Pack-JNTracy` | `761bcc31f406` | 0 |
| `C:/CodeProjects/GodotProjects/tracy-0.13.1` | `dev` | `3947331da927` | 0 |
| `tracy-0.13.1-fixed-slices` | `feature/JNTracy-FixedSlices` | `2b32e8316e77` | 82 |
| `tracy-0.13.1-n30-long-trace-session` | `feature/JNTracy-N30-LongTraceSessionStore` | `8df7b0a19e08` | 0 |

证据：[本次仓库快照](evidence/repository-snapshot.json)。固定切片存在未提交实现，交接时只提供 HEAD 会遗漏这些内容。

当前可定位的 Tracy 工具包为 [Tracy-Integrated-3947331da](C:/Users/Admin/Documents/JN-Unity-T3/Tracy-Integrated-3947331da)，共八个 Windows x64 Release 程序，构建时关闭 LTO。本轮八个 EXE 与构建清单的 SHA-256 均一致。[哈希复核](evidence/tool-hash-check.json)

对应合并验收记录为：123 项功能回归、真实短录制、3 项恢复测试通过；大规模和深度 stream 测试明确未包含在这轮范围内。[验收记录](C:/Users/Admin/Documents/JN-Unity-T3/Tracy-Integrated-3947331da/validation.json)

已安装和 Tracy 仓库中的诊断 Skill 版本号均为 2.3.1。本轮未逐文件证明两份 Skill 完全相同。它仍要求 Query schema 1.35.0、普通 `.tracy` 输入，并对 stream 单独路由；底层支持 stream 直接查询，不会自动使旧 Skill 合同同步更新。

最新独立 Player 为 `T3_development_JNTracy_20261009-203345`，部署 manifest 记录构建与静态校验完成、`runtime=pending`。它使用了为 2375 原包准备的音频与渲染等兼容包映射；不能仅用当前 PackageRepo HEAD 解释所有实际编译内容。[最新打包聊天](codex://threads/01a120a1-ff7c-76c2-b8b0-d410a7b16dad)

## 三 各条工作主线的内容

### 1 选型和总体架构

早期工作包含 Tracy 原理学习、Godot D3D12/GPU/FrameImage 原型、SQLite 导出与语义验证，以及 Unity 接入方案。随后 AI 的主要入口转向 Query/MCP，并建立面向真实项目的采集、诊断与交付流程。

开发交接应保留几个设计理由：尽量复用已有 Unity 标记；用专用事件补足 Job、资源、GPU 与 I/O 等结构化信息；让采集端承担有预算约束的记录工作；让查询侧计算统计；让 AI 解释证据并提出可检验假设。

“早期 SQL 导出不再作为 AI 主流程”是历史架构选择，不等于当前实现不能使用 SQLite。10 月新增的 EventStore 本身使用 SQLite 与 LZ4；两者处于不同层次。[架构说明](../2026-09-document-center/01-system/architecture.md) · [早期流式链路](codex://threads/019f87e8-8291-70b2-a618-782a59bc6878) · [最新分支说明](codex://threads/01a11f9d-389d-7e21-ae93-8b9275b568c2)

### 2 Unity 采集和执行关系

引擎侧负责唯一 JNTracyClient、生命周期与配置，以及 Profiler Marker、线程、帧、Job、GPU、Native 内存、I/O 和系统观测的接入。Package 侧补充 C#/Lua、渲染 Pass、Managed 资源信息和项目自动化。

这部分已经历多轮正确性修复：Main.PlayerLoop 根区间错误嵌套、Render Thread 边界和覆盖、Marker 配对、重连代际、资源指针复用、源码位置和调用栈身份等。接手者需要理解修复所保护的语义，避免仅根据事件名字重建关系。

必须区分 Source Location、Native Callstack 和 Managed/Lua Source Stack；栈深度是配置的一部分，不证明每个事件都有独立动态调用栈。`Frames`、`Player.Frame`、`Render.Frame` 也必须分别标明，不能按相同序号互换。[CPU 与 Job 链路](../2026-09-document-center/02-chains/03-cpu-job.md) · [Render Thread 修复验收](C:/Users/Admin/Documents/JN-Unity-T3/Render-Thread-Zone-Nesting-and-Coverage-Fix-Acceptance-20260906.zh-CN.md)

最新引擎分支说明还记录了线程内联 Writer、后台 Relay、延后元数据格式化，以及部分高频短 Marker 的按帧聚合。交接需要解释它们如何改变“逐事件明细”和“聚合数据”的可得性；历史逐事件分析规则不能直接套用。[最新分支说明](codex://threads/01a11f9d-389d-7e21-ae93-8b9275b568c2)

### 3 GPU 计时和显存资源归因

GPU 方向包括 D3D12 Direct/Compute/Copy Queue、CommandList 与 Segment、L0/L1/L2 层级、显式 Pass、OriginFrameId 和 DXR 计时。资源方向覆盖 DXGI 用量与预算、物理分配、逻辑资源、Heap/Range、Texture/Mesh/VG/RTAS，以及 Pass 或 Frame 到资源的引用。

N26/N26-R 探索和修复资源来源图，N27 建立 GPU Resource Catalog，N28 推进 Schema 3 双向资源证据，N31 关注 Managed Catalog 开销和生命周期。这些阶段的真实覆盖、性能结果和发布范围不同，不能统一标为“全部验收完成”。N28 历史聊天报告实现与本机可执行验收通过，但生产外部门禁仍为 Partial；其旧报告路径在当前引擎分支不存在，应通过对应历史分支找回。[资源演进](../2026-09-document-center/03-iterations/resource-evolution.md) · [N28 历史结论](codex://threads/01a0224d-ecc3-73c0-8f22-0b3593dea589)

当前 dev 已增加显存分类、CPU/GPU 使用率、FPS 和按帧 GPU 资源查询。需要保留的分析口径是：CPU 提交时间与 GPU 执行时间不同；逻辑大小不能与物理占用相加；被某帧引用不等于该帧独占；采样区间的利用率不能当作单帧精确值。[GPU 链路](../2026-09-document-center/02-chains/04-gpu-frame-dxr.md) · [资源显存链路](../2026-09-document-center/02-chains/05-resource-memory.md) · [指标口径](../2026-09-document-center/06-quality/metrics.md)

### 4 采集配置和自动化

已建设 Daily、HighEvidence、GPU Reference、ExactCallstack、Lua、Startup I/O/Memory、SystemSampling 等档位或专项预设，以及自动登录、业务场景就绪、连续稳定帧和自动采集。档位名称不能代替各数据域开关、预算和实际质量。

自动登录有一项必须保留的纠正：N20/N21 的成功结论已作废；N22B 才通过当前 loginAdapter、player/space、业务场景和连续 60 帧就绪检查。同一轮 HighEvidence 全域数据质量仍失败，登录通过与数据全域通过应分别记录。[自动登录验收原件](C:/Users/Admin/Documents/JN-Unity-T3/deployment-records/20260816/Player-T3-Auto-Login-and-Capture-Acceptance-2026-08-16.md)

Capture GUI 已合入 dev。历史样本曾因连接已运行的 Player 而实际使用 Daily；在 GUI 中选择 HighEvidence 不会修改目标进程已读取的配置。需要在启动与录制之间绑定 PID、配置路径/哈希，并读取 AppInfo 的 effective config。[配置不一致案例](codex://threads/01a0e753-ce39-79f3-872d-934e2a3ae7da)

10 月 10 日对当前引擎源码配置的核对发现：

| 配置文件 | 当前文件内容 | 交接影响 |
|---|---|---|
| `JNTracy.ini` | `Preset=HighEvidence`、Detail、2000 Hz、Frame 粒度 GPU 引用、Catalog 关闭 | 已不能按旧文档解释为 Daily；关闭 Catalog 不等于关闭全部显存和帧资源数据 |
| `JNTracy.HighEvidence.ini` | HighEvidence、Detail、2000 Hz、Catalog 开启 | 与默认文件并非相同配置；需按分析目的选择 |
| 配置 README | 仍写默认 INI 是 Daily 的别名 | 文档与当前 INI 有漂移，应在正式使用手册中修正 |

两个 INI 当前均为 `UnityMarkerDepth=0`，不能沿用旧材料中“HighEvidence 的 Unity Marker 栈深为 8”的通用说法。实际运行仍以构建产物旁的配置及捕获回执为准。[源码配置目录](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Config)

### 5 存储转换和长录制

这一主线经历了多条路线，交接时应单独画版本图：

| 路线 | 解决的问题 | 已知状态与边界 |
|---|---|---|
| Journal 与 `.tracy-stream` | 持续落盘、提交尾、校验和异常尾恢复 | 早期纵向链路已完成；原始流包含协议与定义依赖，不能按字节任意切开 |
| Legacy/Offline 转换与 JNTRACYX | 将 stream 转成可查询快照，降低重放和全量驻留成本 | 有短样本全域语义对比及 30 分钟转换验收；不同文件格式需匹配 reader |
| N29 GPU sidecar | 避免反复全量构建 GPU 资源分析 | 历史为带声明例外的通过，不代表全部长录制路径完成 |
| N30/N30A Session Store | Canonical 事实分片、Derived 索引、Audit、generation 发布 | 曾完成指定 A7 长录制验收；后续 P3 双路径差分失败。当前完整功能仍不在 dev |
| 10 月新 EventStore | 采集时建库，Profiler/Query 直接访问 stream | 已合入 dev 并完成日常功能测试；大规模完整性仍有明确限制 |
| 固定时间窗切片 | 让各片可独立使用，同时保留原始身份、跨界事件和状态 | 独立功能分支实施中，总体验收未完成 |

N30 的 Session 是容器，Canonical 是权威事实层，Derived 是可重建索引；它们不应作为三个并列文件格式介绍。N30 的历史提交 `7eb9e55d`、`8df7b0a1` 均不是本次本地 dev 的祖先。[存储演进](../2026-09-document-center/03-iterations/storage-evolution.md) · [N30 查询差异](codex://threads/01a0719c-b969-70f0-8c27-525114eeec86)

固定切片的最新实施记录包含真实短流 13/13 片 CPU 投影差分、13/13 片 Frame 投影检查，以及一次 66/66 Query 回归；之后还有普通 frame.list 范围修复的 3/3 定向通过。它们针对不同检查点。全域字段、原始大源、完整 GUI 消费者、匹配 Release 工具和原始 32 片最终验收仍未完成；不得把这些绿色结果拼成全量通过。[固定切片计划](C:/Users/Admin/Documents/JN-Unity-T3/Tracy-Fixed-Slice-Plan.zh-CN.md) · [最新实施证据](C:/Users/Admin/Documents/JN-Unity-T3/Tracy-Fixed-Slice-Evidence/20261007-mcp-parity-001/README.md)

### 6 Query 和 MCP

Query 逐步承担 Trace 身份与质量检查、结构化查询、分页、关系导航、确定性扫描、精确统计、异常和预算策略。QSCAN 将统计数值集中到 Query：AI 定向调查，Python 维护状态和渲染报告。Analysis Profile、Neutral Aggregate 与 Candidate Policy 分开后，可在 Profile 变化时复用中立统计。

缓存工作还包括分块压缩、过程表回收、外部排序、持久状态、断点恢复、磁盘和内存预算。历史缓存实验将某一分析目录从 32.56 GiB 降到 2.10 GiB，但不能据此认为计算耗时按相同比例降低。[Query 缓存讨论](codex://threads/01a09f47-1097-70f3-b3d7-d8daae7f431e)

R20 在冻结的 12.43 GB Trace 和已发布 schema49 索引上完成 Scan 52 分 04.117 秒、总流程 74 分 12.541 秒。它复用了 R14 索引，不是新的冷建验收；源数据质量和 enrichment 缺口仍被保留。实现提交已进入当前 dev 的历史，但这不能证明 10 月新访问层完全继承旧运行的性能。[R20 报告](C:/Users/Admin/Documents/JN-Unity-T3/GpuCatalog-DiskIndex/Query-Final-Acceptance-20260929-r20/Report.zh-CN.md)

最新合并聊天记录在 11 份样本上完成 56 次 Query 和 45 次 MCP 查询；有、无 EventStore 的 stream 都能打开。存在配套 store 时优先读取，没有时可能重放并生成快照/索引。该记录同时指出以下限制：单次帧/范围资源查询最多读取 1,048,576 条引用；全局 GPU 内存归因在超过 1 亿引用时省略明细；部分快照导出仍有源码位置数量边界；缺少配套数据时返回 `CAPABILITY_UNAVAILABLE`。大规模阈值来自当时代码核对，未做对应规模压力测试。[stream 访问与限制](codex://threads/01a11fc4-f913-7cc2-a0a8-4c2092d7289a)

### 7 AI 分析 Skill 和报告质量

诊断 Skill 从 1.x 的事件查询与图表流程，演进到 Query 原生扫描、Schema 4 状态、候选身份与完整回执，再到 2.2 优先级分流、2.3 原生视图、2.3.1 阅读分册。当前合同是：新 P0 强制源码深查，新 P1 先交测试人员决策，不把所有候选都标为已完成深查。

当前 2.3.1 要求总览导航、详细观察每五条分册，并将分析、源码、图片、Profiler 链接放在同一条内。Skill 自己明确声明：分册与配图合同尚未全部进入通用生成器和验收器。历史报告中的临时生成脚本不能被当作正式入口已实现的证据。[安装版 Skill](C:/Users/Admin/.codex/skills/diagnose-jn-unity-tracy/SKILL.md)

10 月质量复盘应作为开发交接的重点：

| 问题 | 已有具体证据 | 后续需要落实的约束 |
|---|---|---|
| 代表实例未对应触发成本 | 曾调查 0.009 ms NativeSingle，而同帧有 130.606 ms 长任务及 129.584 ms 等待 | 触发指标、代表实例、等待完成端必须对应 |
| 正常对照不可比 | 689 ms 长帧内部的 8.38 ms VSync 区间被当作正常对照 | 校验 FrameSet、真实帧边界、场景和工作负载 |
| 下一步缺少针对性 | QS16B 的 818 条观察、QS17 的 73 条观察各使用同一句下一步 | 每项写具体假设、待查数据和可区分原因的复测 |
| 重要发现从正文消失 | 长 Job 与主线程等待关系只存在于被隐藏的发现过程 | 正式正文必须保留关键发现和依据 |
| 数值校验缺少语义 | 把纳秒成本标为调用次数的单项复测被旧校验接受 | 同时校验数值、单位、统计范围和聚合方式 |
| 插桩范围过粗 | Shader 创建区间包含锁、复制、设备调用、发布等阶段 | 先核查已有信息在哪一层丢失，再补可区分假设的分段和关联信息 |

以上来自历史报告复盘，不表示每份报告或每条结论都有这些错误。最新讨论提出“结论与影响、样本与证据、机制与源码、假设与反证、处理方案、复测标准”六段结构，并保留领域专用表；目前属于讨论方案，尚未修改 Skill。[深度评审](C:/Users/Admin/Documents/JN-Unity-T3/Skill-Quality-Review-20261008/Tracy分析与报告质量深度评审.md) · [插桩与格式建议](C:/Users/Admin/Documents/JN-Unity-T3/Skill-Quality-Review-20261008/插桩分析与报告格式约束建议.md)

### 8 Profiler 和报告交付

已经建设原生 GPU/资源视图、线程布局保存、Capture GUI、Wasm 网页 Profiler、本地查看服务、精确定位记录和原生截图导出。网页查看器通过 view-manifest 把报告条目连接到 FrameSet、线程、时间区间和事件。

已有 QS17 2.3.1 独立报告包，包含总览、分册、源码快照、证据、原始 Trace、查看器和运行时。接收者需要完整解压并启动本地服务；`127.0.0.1` 链接不具备跨机器持续有效性。[QS17 交付包](C:/Users/Admin/Documents/JN-Unity-T3/QS17-Skill-2.3.1-SharePackage/README.zh-CN.md)

另一个 Player 报告案例为 53 项强制观察补充了 52 项原生截图与交互入口；剩余原生 Job 对象无法精确定位，保留 degraded。这个案例的数字来自共享对话转述，本轮没有逐项重新审计其报告包。[共享内容整理](codex://threads/01a0e6e5-a318-7572-9fcd-b17011967d63)

线程布局保存曾在待合入分支的差异说明中被列为删除；当前 dev 的 TracyView 已可见 LoadThreadLayout/SaveThreadLayout 调用。因此正式文档应描述最终合并结果，不能照搬合并前某一侧的删改清单。

### 9 构建部署和版本管理

已形成 `jntracy-stage-player-code`：按 Native-only、Scripts-only、Coordinated、Full-runtime 区分构建和替换范围，将代码部署到原 Player 的完整独立副本。GameAssembly、metadata、Burst、初始化清单和 PDB 需要保持同代，完整运行副本与精简 build seed 要分清。

实际经验包括：同为 Unity 2022.3.67f1 不代表源码、绑定、序列化布局和资源契约相同；源码 Editor 构建可能触碰 P4 本地资产；原发布包、临时项目设置、第三方插件和资源必须按部署合同处理。旧线程中“未操作 P4”后来被纠正为“未操作服务器状态，但本地 workspace 被 Unity 改写”，应保留这个教训。[部署 Skill](C:/Users/Admin/.codex/skills/jntracy-stage-player-code/SKILL.md) · [P4 本地变化案例](codex://threads/01a06173-6263-78e1-9f40-f6e79f25b4f9)

该 Skill 的现有 PowerShell 部署脚本只支持 Full-runtime；其他模式依赖显式部署流程。最新 2375 试验包已生成但未启动，应将其运行验证作为交接待办，而不是默认可用基线。测试机上也必须重新解析路径和 MCP 配置，不能直接复制开发机绝对路径。[测试机说明](C:/Users/Admin/Documents/JN-Unity-T3/TestMachine-Handoff/2026-09-14/README.zh-CN.md)

### 10 数据验证和知识维护

验证资产包括 Native/Query 合约测试、真实 Editor/Player 采集、snapshot/stream 语义差分、故障注入、取消/恢复、资源预算，以及 Unity Profiler 与 Tracy 的全线程、名称和父子路径对账。

Job worker 对比证明：`RenderLoop.DrawSRPBatcher → SRPBRender.ApplyShader → SetShaderParameters` 在两份录制中都存在，先前可能让人误认为“路径迁移”的表述已纠正。另有传统阴影路径未见；源码和修复包存在相应 Marker，不能只凭一份录制没看到名称就判断未插桩。两次录制的工作量、运行分支、二进制身份和 Marker mismatch 都会影响解释。[精确层级核查](C:/Users/Admin/Documents/JN-Unity-T3/UnityProfiler-Tracy-JobWorker-20260928/SRPBatcher-Hierarchy-Audit.zh-CN.md)

采集自身开销也有独立历史实验。N16 待激活开销的严格统计门槛未通过，但不能把置信上界直接解释为 Tracy 的固定真实开销；保留实验噪声、样本和 WPR 证据，才能继续改进实验设计。[开销实验说明](codex://threads/019fc264-0bc1-7d42-9132-e6eff20daff3)

本轮在现有文档中心盘点到 36 份既有 Markdown，根目录另有导航文档，包含系统、七条链路、迭代、分支、操作、质量和协作规则；另外还有中文手册、早期方案、案例报告与部署档案。应在既有体系上更新状态和补齐操作闭环，减少新的重复版本。[原文档中心](../2026-09-document-center/README.md) · [中文手册入口](C:/Users/Admin/Documents/JN-Unity-T3/output/tracy-manual-zh-CN/README.zh-CN.md)

## 四 需要保留的开发演进

| 阶段 | 核心变化 | 交接时的状态说明 |
|---|---|---|
| 7 月原型与接入 | Godot/D3D12、流式 Journal、Unity 单 DLL、Editor/IL2CPP Player、Query/MCP | 形成基础闭环，早期配置和路径不能直接作当前使用步骤 |
| N00 至 N16 | 身份、质量、GPU、Managed/Lua、Job、I/O、低开销路径 | 专项功能通过与生产性能矩阵结果分开 |
| N17 至 N25 | 帧根、Player 正确性、自动登录、HighEvidence、引用集合、调用站点、FrameImage、stream-only | 保留 N20/N21 结论作废和后续修正 |
| N26 至 N31 | ResourceGraph、Catalog、来源图、GPU sidecar、Session、Managed 开销 | 各分支路线与验收范围分别交接 |
| 9 月 QSCAN 与 Skill | 确定性统计、窗口/候选、缓存、源码调查、优先级分流、原生视图与分册 | 文档中心尚未充分覆盖后续 2.3.1 和 R20 状态 |
| 9 月后半数据一致性 | Unity Profiler 对账、Marker 完整性、报告查看器、长录制转换与 Query | 数据正确性、工具可用性、分析质量是不同验收对象 |
| 10 月当前主线 | EventStore、stream 直接访问、显存分类/利用率/FPS、固定切片、Skill 质量复盘、新 Player 试验 | 已合入 dev 的能力、独立分支开发和仅讨论的方案分别标注 |

## 五 交接待办和未关闭事项

以下是需要转交的工作，不是本轮追加开发任务。

| 顺序 | 待办 | 当前依据与明确完成条件 |
|---|---|---|
| 优先 | 确定正式交付版本组合 | 以三仓 SHA、实际包映射、八个工具哈希、Skill 内容、Player manifest 和 effective config 形成同一套基线 |
| 优先 | 验证最新 2375 Player | 当前仅构建与静态校验完成；补启动、登录、业务场景、采集与数据质量记录，保留兼容包例外 |
| 优先 | 修正文档和配置漂移 | 更新 9 月旧 STATUS、Skill 输入路由说明、默认 INI 与独立 HighEvidence 差异；不从文件名推测实际能力 |
| 优先 | 完整移交固定切片进行中状态 | 保存当前 82 项 tracked 修改及另行核对的 untracked 内容、构建身份和恢复入口；继续全域与实际消费者验收 |
| 随主线推进 | 固定 Skill 报告合同并落实生成器 | 基于新 dev 的代表性录制试验六段结构，落实实例、对照、指标语义、正文关键发现和原生展示验收 |
| 随主线推进 | 核对新访问层的容量与完整性 | 验证 partial/truncated/warnings 的传播、分页、源码位置与引用上限、长录制重开/恢复；与旧 R20 分开记录 |
| 待明确方向 | 处置 N30 Session 路线 | 完整 N30 当前未合 dev，P3 差分仍有遗留；明确继续、仅保留历史复现，或由新路线接替 |
| 待核对继承关系 | R20 旧遗留项 | 冷建恢复、跨进程磁盘账本、GPU Frame Evidence、Ghost 全量 MCP 导出未被当时验收关闭；新增帧资源接口是否覆盖同一合同需逐项对应 |
| 持续维护 | 数据质量与开销 | Catalog enrichment、Marker mismatch、真实来源覆盖及性能对照按样本和版本核验，不从总计通过数推导全域无损 |

## 六 交接文档建议如何组织

面向开发维护者，建议在已有文档中心形成下列可独立使用的交接件。此表用于下一轮确定范围，当前尚未替代为全部正式手册。

| 文档 | 核心内容 | 现有基础 |
|---|---|---|
| 交接总览和版本基线 | 仓库职责、当前版本组合、可用产物、未完成事项、阅读顺序 | 本稿、STATUS、仓库与工具快照 |
| 架构和关键链路 | 采集、执行关系、GPU/资源、存储、Query、AI、交付；每条链的身份、失败和源码入口 | `01-system`、`02-chains` |
| 采集与分析操作手册 | 配置选择、Player 启动、自动登录、Capture GUI、停止、MCP、Skill、报告回查 | `05-operations`、两项 Skill、自动登录验收 |
| 构建部署和兼容手册 | 三仓分支、包映射、Engine/IL2CPP 成套构建、资源契约、PDB、测试机安装 | staging Skill、部署 manifest、测试机档案 |
| 数据契约和验证手册 | FrameSet/ID/时间单位、partial 状态、资源口径、差分、故障注入、开销实验 | contracts、metrics、validation、对账资料 |
| 开发迭代和待办交接 | 各路线为何产生、最终成果、失败纠正、继续开发的分支和验收条件 | `03-iterations`、专题实施记录、本稿第五节 |
| 典型案例集 | Shader/Job 等待、GPU 工作集、Marker 对账、长录制、配置错误、部署兼容 | AnalysisReports、QS16B/QS17、质量复盘 |
| 来源与资产清单 | 聊天、文档、样本、工具、manifest、哈希、失效路径与迁移记录 | [本次来源索引](sources.zh-CN.md)、原始 evidence 和 History |

下一轮讨论建议先确定正式交付版本组合，再确定 N30 与固定切片的交接定位，最后按上述目录选择需要补写到可直接执行的章节。这样开发者能接续工作，使用者也能按一套明确版本完成采集和分析。
