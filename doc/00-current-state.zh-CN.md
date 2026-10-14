# 当前版本与交付状态

基线日期：2026 年 10 月 14 日。本文优先回答接手者“现在应看哪一套代码、运行哪一条流程、哪些验证可以引用”。

## 三仓基线

| 仓库 | 本地目录 | 指定分支 | 冻结 HEAD |
|---|---|---|---|
| Tracy | `C:/CodeProjects/GodotProjects/tracy-0.13.1` | mainline | `f49190f2541c8da739d6c74fd2fe34eeff222e5e` |
| Unity | `C:/workflow/jnunity` | feature/JNTracyUnity | `c2877d4b053c3e084b0316fc08bc6a1209e2baa8` |
| Package | `C:/workflow/PackageRepo` | feature/Pack-JNTracy | `0ac6735ce3ca84f7ffbe2dd0cfcfa8acbfa2ed91` |

采样时 Tracy 的 31 个原 doc 文件处于工作区删除状态，Unity 有未跟踪的个人配置，Package 无工作区改动。本次在 doc 建立新目录，旧删除保持不变；历史原文从已登记版本归档到 history。以上 Git 信息均为本地 refs，未执行 fetch。[仓库快照](evidence/repository-snapshot.json)

## mainline 和其他分支的关系

本地 dev 是 mainline 的后继，另有 4 个提交：`f62bd2d69` 的归并文件流与 CPU 窗口扫描修复、其合并提交、`d261d1288` 的分析器内存及字符串比较修正，以及合入 mainline 的合并提交。最终树相差 8 个文件。

mainline 的分析器源码固定私有内存上限为 **16 GiB**。dev 的后续修订和安装版的 **32 GiB** 不能合并描述为同一个版本。当前两份 EXE 的 help 均没有进程内存上限参数；`--vram-limit-bytes` 是被测程序显存分析阈值，不是分析器内存上限。[analyze.cpp:296](../query/src/analyze.cpp#L296) [分支关系](evidence/branch-relations.json)

N30 Session Store 和 FixedSlices 仍是独立历史/开发分支，不能通过它们的旧验收自动扩大 mainline 支持范围。完整 Session Store 路线不等同于当前 EventStore；固定切片仍需按它自己的状态与消费者验收交接。

## Skill 和二进制基线

| 位置 | 包版本 | 用途 |
|---|---:|---|
| `skills/diagnose-jn-unity-tracy` | 2.3.1 | 普通 .tracy 的旧 Query/MCP 合同与历史工作流 |
| `skills/diagnose-jn-unity-tracyV2` | 2.6.3 | 新 Stream 路由、原因调查、逐问题原生网页回查，同时保留旧路由 |
| 本机 `.codex/skills/diagnose-jn-unity-tracy` | 2.6.3 | 当前本机使用版本，含本机配置和额外修订的分析器 |

V2 和安装版有 167 个共同路径，其中 165 个字节相同；差异是分析器 EXE 与 config/.gitignore。安装版另有 6 个文件，包括本机配置和分析器构建记录。相同 frontmatter 版本不能代替逐文件或 EXE 哈希。[Skill 清单](evidence/skill-inventory.json)

| 程序身份 | 当前已知事实 |
|---|---|
| V2 附带 tracy-analyze.exe | SHA `49c7a53af1a70878f3410a2cab113bbcf695f5928cb371fe6edd4de137e6eedd`；安装补丁记录把同哈希旧程序识别为 4 GiB 版本 |
| 本机安装 tracy-analyze.exe | SHA `e707bab79af9caf40b04566ccc04e3739fd58a63c7684f1d55896916ae6eaf74`；32 GiB 修订，来自 dev 基线加明确补丁 |
| V2 与安装版 offscreen Profiler | 相同 SHA `9c18b11a105016a5a373a4c766c68f6c6dd3744925f53c4718323b37291c5288` |
| mainline 当前分析器源码 | 16 GiB 固定上限；本轮没有生成相应新 EXE |

安装版 EXE 与其本机配置中的预期哈希均匹配。[工具核对](evidence/installed-tool-identities.json) [tracy-analyze-build.json:1](C:/Users/Admin/.codex/skills/diagnose-jn-unity-tracy/bin/tracy-analyze-build.json:1)

## 当前工作流状态

| 能力 | mainline 实现与使用入口 | 可引用的验证范围 |
|---|---|---|
| 录制与保存 | Capture GUI 运行 capture 和跟随式 EventStore builder；最终发布 stream/store | 有既有集成、短录制和恢复记录；以具体工具 manifest 为准 |
| Stream 读取 | schema17 EventStore；Query、analyze、StreamingView 使用 | 10 月 13 日完整分析已保存，但分析器为安装修订版 |
| 普通 Trace | Worker/QueryIndex、Query/MCP、旧 Skill | 与 Stream 的分类、统计、状态合同独立 |
| 全量候选与严重帧 | frame-triage-v4.1，完整候选账本和独立审计 | 同一真实录制 31,373 个完整帧、422 个严重帧的覆盖实例 |
| 原生网页回查 | 隐藏 native renderer、网页 framebuffer 与输入通道 | 功能、定位、响应性、显示比例分别有要求，不能以一项代替全部 |
| 源码根因和收益 | AI 定向调查、源码快照、反证与复测计划 | 完整扫描不等于全部因果关系或收益已验证 |

## 引擎和 Package 当前状态

当前 C ABI 为 **1.8（0x00010008）**；Package 的 ExpectedAbiVersion 也是 1.8。Tracy 协议为 90，程序源码版本为 0.13.2，目录名中的 0.13.1 不是当前程序版本。[JNTracyApi.h:22](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Public/JNTracyApi.h:22) [JNTracyRuntime.cs:124](C:/workflow/PackageRepo/com.jngame.tracy/Runtime/JNTracyRuntime.cs:124) [TracyProtocol.hpp:12](../public/common/TracyProtocol.hpp#L12) [TracyVersion.hpp:1](../public/common/TracyVersion.hpp#L1)

默认 JNTracy.ini 标为 HighEvidence，启用 Frame 粒度引用、关闭 Catalog；独立 HighEvidence 文件开启 Catalog。两个文件不能只按 Preset 名称视为等价。配置 README 仍写 Daily 别名，是明确的说明漂移。[配置矩阵](appendices/configuration-matrix.zh-CN.md)

最新已定位的 Player 已从 2375 试验包推进到 **2453 完整独立副本**，随后对 Client DLL/PDB 做了限定修复。已有 5 秒实包后台录制、转换、索引和结构校验，不能写成完整前台场景与长时间稳定性全部通过。[部署案例](14-validation-and-real-cases.zh-CN.md)

## 正式交付前需要确定

- 选择 mainline 新构建还是已验证安装版 EXE，保存来源提交、补丁、配置和哈希；不要让安装器意外把 32 GiB EXE 覆盖回旧包二进制。
- 核实最新 Player 实际加载的 Client、UnityPlayer、GameAssembly 与包映射；当前三仓 HEAD 不自动等于某次录制使用的构建。
- 按本轮任务需要分别验收数据完整性、分析覆盖、因果解释和展示能力。
- 对 dev 独有修复、旧 Session 路线和固定切片作明确的后续处理决定。
