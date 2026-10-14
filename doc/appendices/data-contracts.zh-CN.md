# 数据对象 身份和统计契约

精确字段以对应版本 describe/schema 和原生回执为准。不同路线的相似字段不能只按名称合并。

## 版本矩阵

| 层次 | 当前值 | 约束 |
|---|---|---|
| Tracy 源码版本 | 0.13.2 | 不由目录名决定 |
| Wire protocol | 90 | Client 与接收端协议 |
| JN C ABI | 1.8 / 0x00010008 | DLL、Native 调用者与 PInvoke |
| EventStore | 17 | 库布局和读取兼容 |
| Query schema | 1.35.0 | 查询和回执合同 |
| Stream classifier | frame-triage-v4.1 | 分类与缓存身份 |
| Stream audit | stream-analysis-audit/v1 | 保存输出的独立核验 |
| 普通 Trace 旧流程 | Schema4 / candidate-policy-v3 | 旧状态、策略与调查合同 |
| Skill 包 | 2.3.1 / V2 2.6.3 | 编排规范，不自动改变二进制 |

[TracyVersion.hpp:1](../../public/common/TracyVersion.hpp#L1) [TracyProtocol.hpp:12](../../public/common/TracyProtocol.hpp#L12) [JNTracyApi.h:22](C:/workflow/jnunity/Runtime/Profiler/JNTracy/Public/JNTracyApi.h:22) [TracyEventStore.hpp:14](../../stream/src/TracyEventStore.hpp#L14) [SKILL.md:1](../../skills/diagnose-jn-unity-tracyV2/SKILL.md#L1)

## 对象与关联身份

| 对象 | 必须保留 | 常见混淆 |
|---|---|---|
| Trace | 内容 SHA、source/revision、实际配置、构建 | 路径相同不证明内容相同 |
| Frame | FrameSet、索引与起止、完整性 | Query index、GUI 显示号、Player.Frame 编号 |
| CPU Zone | owner、完整父路径、source/dynamic name、端点、ref | 同名函数与同一调用实例 |
| Job | handle/generation、traceId、span/阶段、依赖 | 类型与实例、执行与等待 |
| GPU Segment/Pass | queue/context、CommandList、timestamp、父层级 | CPU 提交、GPU 执行、游戏帧 |
| Resource/Allocation | generation、生命周期、大小/类别、物理根与绑定 | 指针复用、逻辑与物理 |
| I/O | request/parent、阶段、结果、上下文 | 同名或同时发生的其他请求 |
| Candidate | 原生 ID、理由、范围、策略/算法与输入 | 报告条目数和根因数 |
| View | capture/EXE、FrameSet/owner、事件、端点、回执 SHA | Store ID、Query ref 和 GUI 帧号 |

跨线程或跨域因果优先采用 typed relation。名称、时序接近与父子包含可以支持假设，不能代替真实等待完成端或资源归属。

## 单位与聚合

- 纳秒用整数或十进制字符串保持精度；毫秒仅为注明口径的展示换算。
- inclusive/self 区分完整事件和帧内交集；self 依赖合法层级，不自动等于 on-CPU。
- count 明确是调用、对象、实例还是窗口记录数量；不能用 ns 代替次数。
- 分位数记录总体、算法、尾帧规则；Stream 统一引用本轮 triage 完整帧口径。
- 父子、不同线程、不同 Queue 和 CPU/GPU 异步时间不直接相加。
- GPU union/root span 是信号，不自动代表帧关键路径。
- DXGI Local/NonLocal、物理、逻辑和引用工作集不能相加为一个总量。
- GB 与 GiB 分开；VRAM 阈值、进程 private guard、store cache 和系统 commit 是不同预算。

## 缺失和完成

| 状态 | 含义 |
|---|---|
| present=false / unavailable | 输入缺少数据或能力，不代表无性能问题 |
| unknown/null | 无法确定，不能填零 |
| partial/truncated/warnings | 当前范围不足，补齐分页/证据或保留缺口 |
| clean end | 采集正常结束，不保证每个业务生命周期自然结束 |
| candidate enumeration complete | 可观测候选账本完整，不保证源域完整 |
| full triage verified | 指定扫描输出通过审计，不包括全部补证/源码/展示 |
| source review complete | 规定调查范围完成，不代表根因与收益全部确认 |
| presentation passed | 指定定位、图片或交互通过，不证明分析正确 |

Query 的只读语义针对原始事实和受限接口；派生索引、缓存、skeleton 或分析结果仍可能写盘。旧“不创建 SQLite/sidecar”说明不能代表当前全部路径。

## 恢复和缓存

Stream 缓存绑定 source/data identity、schema/revision、策略和 EXE SHA，每个文件检查长度/SHA。旧 Query 的 Neutral/Policy 有独立身份规则；两条路线不能共用完成标志。游标属于 session/revision，不是跨进程持久身份。
