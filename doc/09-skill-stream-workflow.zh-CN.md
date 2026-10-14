# Stream Skill 完整分析流程

适用于已有 .tracy-stream 和已发布 schema17 EventStore。目标是完整筛选可观测性能线索，再针对明确的帧、线程、函数、资源和依赖调查原因，输出可审计和可回查的报告。

## 1 输入和身份预检

确认实际捕获文件、store、EXE SHA、可写的新输出目录、目标 FPS/显存阈值、源码根及已知版本。用户给目录时先定位真实文件；多个不明确输入才需要选择。工具缺失、哈希不符、store schema 不支持时，记录原因，不能偷偷改用旧流程后声称新流程通过。[stream-analysis.md:1](../skills/diagnose-jn-unity-tracyV2/references/stream-analysis.md#L1)

Stream 模式允许使用原生 CLI；下文命令不受旧“仅常驻 MCP、candidate-policy-v3”的限制，但也不代表旧 Schema4 合同被执行。

## 2 运行全量分析

```powershell
$skill = 'C:\Users\Admin\.codex\skills\diagnose-jn-unity-tracy'
$capture = 'C:\YourCaptures\run-001.tracy-stream'
$analysis = 'C:\YourAnalysis\run-001'
python -X utf8 "$skill\scripts\stream_workflow.py" run `
  --capture $capture --output $analysis --no-cache
```

先配置同目录的机器 local 与 profile，或显式给 `--exe`、`--expected-sha256` 等参数。默认 profile 为 60 FPS、5 GiB、64 代表和 4 workers；运行保存的 effective_policy 是本轮权威。

wrapper 只编排程序、保存回执和核验结果，不读取全部 Trace 重新计算统计。全量 diagnose 覆盖已记录的 Main/Render/Worker、GPU queues 和显存；shortlist 不具备同等 CPU/GPU 覆盖。[stream_workflow.py:1](../skills/diagnose-jn-unity-tracyV2/scripts/stream_workflow.py#L1)

## 3 先审计候选，再扩大取证

```powershell
python -X utf8 "$skill\scripts\stream_workflow.py" audit `
  --run "$analysis\native-run" --output "$analysis\audit.json"
python -X utf8 "$skill\scripts\stream_workflow.py" evidence-plan `
  --run "$analysis\native-run" --output "$analysis\evidence-plan" --all-severe
```

audit 核对文件 SHA、帧分母、候选 ID/理由、严重帧保留、预算理由、分页和代表覆盖。需要区分：程序执行成功、候选枚举完整、各源域完整、因果已确认。

64 代表是展示/取证预算，不是“只分析 64 帧”的许可。evidence-plan 将严重帧窗口按 64 个一批输出，并保留长 CPU 事件树、独立 GPU 长区间和显存信号。超过预算的部分进入明确的追加计划或 backlog，不能标为正常。[stream-analysis.md:1](../skills/diagnose-jn-unity-tracyV2/references/stream-analysis.md#L1)

## 4 追加窗口与 Query 请求

使用同一个已验证 EXE，给定计划和新输出目录：

```powershell
tracy-analyze.exe --trace C:\YourCaptures\run-001.tracy-stream `
  --skip-triage --plan C:\YourAnalysis\run-001\evidence-plan\实际计划.json `
  --output-dir C:\YourAnalysis\run-001\enrichment-001
```

计划文件名来自工具真实输出，不按示例猜测。追加证据无需重跑全部扫描；它单独通过也不代表候选全量枚举通过。保存批次索引、命令、原生输出和 audit。

## 5 对全部线程建立调查义务

```powershell
python -X utf8 "$skill\scripts\stream_review.py" `
  --run "$analysis\native-run" --output "$analysis\review"
```

该脚本读取已保存窗口，为 Worker 长作用域、等待、资源创建上游、PSO、低单次高频提交等建立义务索引；它不制造新的性能候选，也不自动确认根因。根据严重性、关注和反证决定定向补查，不能只看 Main/Render self 前两项。[stream_review.py:1](../skills/diagnose-jn-unity-tracyV2/scripts/stream_review.py#L1) [cause-investigation.md:1](../skills/diagnose-jn-unity-tracyV2/references/cause-investigation.md#L1)

当前实现只遍历指定 run 的 manifest 登记的 window-*-cpu.json，并要求 full_triage_verified；不会自动合并其他 enrichment 目录。它返回的 all_thread_owners_considered 只覆盖这些窗口。追加批次的调查义务必须另行登记与核查，不能把这个布尔值当作全部严重帧/所有批次已经调查完毕。

## 6 绑定具体实例

从完整 owner/path 取真实 max_ref，再调用 CPU/GPU tree 查询。ref 为空不猜；不同 capture 的 ref 不复用；分页返回不完整时不把第一页当全部 child。

保留完整事件、帧内交集、self、inclusive、次数及统计范围。一个 130 ms 的跨帧调用可以只在当前帧贡献 51 ms；两者可以同时正确，必须明确标签。GPU 原生区间不强行编号为 Player.Frame。

## 7 源码和原因调查

每个主要问题至少回答：具体线程/函数；业务上游和资源；执行方/等待方；已有缓存、预算或预热为什么没有阻止；正常或反例样本；实际捕获配置；仍缺少的最小区分证据。

Scope 名称不是语义判决。CreatePSO 常是 CPU/driver 工作；Present 可含 CPU 回收；PxScene.simulate 可以包含帮助执行和等待；没有 Wait 字样的 Async API 也可能同步阻塞。[cause-investigation.md:1](../skills/diagnose-jn-unity-tracyV2/references/cause-investigation.md#L1)

源码不匹配捕获版本时可以解释当前机制、提出可验证方向，但不能确认运行时根因。所需信息可能已在采集或 Query 回执中，应先追查字段传递，再决定是否补插桩。

## 8 组装报告与原生回查

总览至少给 P 级、CPU/GPU、Main/Render/Worker/Queue、具体函数、样本与耗时、建议动作、确认程度。详细观察每册至多五项，按真实主要帧序排列；每项就地提供分析、源码、反证、原生图片和同一视图链接。

报告独立披露 scope_located、mechanism_supported、runtime_cause_verified、benefit_measured。完整覆盖与分析深度各有状态，不能相互替代。展示流程见 [原生回查](12-profiler-views-and-share.zh-CN.md)。

## 9 输出和恢复

主要材料包括：native manifest、capture-context、frames/candidates/native-candidates、窗口与请求回执、audit、调查义务/账本、源码快照、view-manifest、presentation-validation、报告与分析过程记录。

同输入/策略/EXE 的完整 triage 阶段可复用缓存或 resume；身份、checksum 或策略变了就不能沿用。恢复到新目录，不复用旧 session 的游标；失败补证按其原计划重做，不为展示失败重扫已验证的数据。

## 10 已有真实完成实例

10 月 13 日的完整运行覆盖 31,373 个完整帧和 422 个严重帧，最终整理 28 条观察、6 册与 33 个原生视图。它使用安装修订分析器；源码匹配、Job 依赖和收益确认仍保留缺口。[README.md:1](C:/Users/Admin/Documents/JN-Unity-T3/Analysis/T3-115721-Full-SKILL-20261013/README.md:1)

同一录制的另一条 Query 替代路线有不同候选口径与观察数量。比较时先统一身份、阈值、分位算法和 full/clipped 定义；不能根据“24 条变 28 条”判断游戏发生变化。[真实案例和对比](14-validation-and-real-cases.zh-CN.md)
