# Stream 原生分析（2.4.0）

适用于已完成录制的 `.tracy-stream` 或包含单个该文件的目录。保留用户的直接读取选择，不转完整 `.tracy`。当前 full 模式可复用小域 metadata skeleton；这不等于全量转换，也不应隐瞒。执行前确认已发布 EventStore、支持的 schema、EXE 身份和可写输出目录。

## 工具与策略

使用已验证、已存在的 `tracy-analyze`。当前实测版本 classifier 为 frame-triage-v4.1、EventStore schema17、Query receipts schema1.35.0。不要自动拿另一个新编译 EXE 替换旧结果的身份。显式 `--exe` / `--expected-sha256` 优先；未给时用 `config/stream-local.json`。路径缺失或哈希不符要明确说明，不改用旧示例路径。`config/stream-local.example.json` 只作结构参考。

`config/stream-profile.json` 默认 60 FPS、5 GiB、64代表、4 workers，用户参数优先。**5 GB=5000000000，5 GiB=5368709120**；EXE同时保存两个参考阈值，不能混淆。旧 YAML 的 module budgets/candidate-policy-v3 未接入此 CLI，不能声称已应用。运行保存 effective_policy 和 SHA 为本轮权威。

```powershell
python -X utf8 scripts/stream_workflow.py run --capture CAPTURE_DIRECTORY --output NEW_ANALYSIS_DIRECTORY --exe VERIFIED_EXE --expected-sha256 VERIFIED_SHA --no-cache
python -X utf8 scripts/stream_workflow.py audit --run EXISTING_NATIVE_OUTPUT --output NEW_AUDIT_JSON
python -X utf8 scripts/stream_workflow.py evidence-plan --run EXISTING_NATIVE_OUTPUT --output NEW_PLAN_DIRECTORY
```

run 是 EXE 编排，不是 Python 统计器。它保存命令/策略/返回码和 native-run；默认四项请求为 trace.info、VRAM summary、plot list、script summary。Python不读取trace/store事件，不重新计算分位、自身时间、排名或性能显著性。

## 先完整筛选，再检查证据覆盖

1. 使用 `diagnose` 全量扫描 Main/Render/Worker、所有 GPU queue、已记录显存；`shortlist` 只适合快速帧/显存预览，不能拿它回答 CPU/GPU bound。记录实际 capture-context（不是 requested.ini）、source/revision、完成域与缺失域。
2. audit 检查清单SHA、全量帧与fast-frame边界、候选ID/理由、严重帧保留、CPU/GPU预算理由、分页和representative覆盖。审计失败时保留证据并修复，不写成健康。`complete`、候选枚举完整、源域完整、因果已确认是四个不同状态。
3. **代表预算不等于卡顿覆盖。** 查 audit 的严重帧未覆盖清单；有缺口时用 evidence-plan 生成追加窗口。它按64窗口分批，不截掉第65个严重帧。还保留原生长CPU树及GPU长区间，不能只按普通帧排序忽略跨帧事件。
4. 主报告至少复核全部严重帧的原生窗口，按同路径实例归并线索，但“同名”不等于已证实同因。对具有不同机制的样本、GPU独立信号、持续慢段和显存超标另行取证。`evidence-plan --all-severe` 可强制生成每个严重帧的精确窗口，便于避免邻帧分组混淆。若严重要查窗口超过64，逐批执行，保持批次索引。
5. 其他候选完整保留在 candidates.ndjson；人工未深查范围与理由写入backlog。自动选中不表示已完成源码根因调查。不要因64代表已满就说剩余帧正常。

追加计划交给原EXE：`--skip-triage --plan PLAN --output-dir NEW_DIRECTORY`，无需再全量扫描。audit 同样可验证这种enrichment输出，但不宣称它单独完成全量候选枚举。

## 函数定位与因果判断

先执行 [原因调查](cause-investigation.md) 的各层检查，并用stream_review.py为全部线程建立调查义务索引。全严重帧窗口通过不代表所有原因完成；尤其不能只展示Main/Render的self前两项后跳过Worker、低单次高频提交和业务上游。

沿 native window 的完整 owner/path 取真实 max_ref，再查 `zone.cpu.tree` / `zone.gpu.tree`；空ref不查询，不复制旧capture的ref。分页回执必须完整，不能将第一页的1000项当作全部children。完整调用、frame-clipped交集、self/inclusive分别保留；跨线程、跨queue及父子不能相加。

特别检查：

- Present 可包含 CPU 回收；CreatePSO/CreateResource 是 CPU 线程内调用，不等于 GPU shader 执行。
- ProcessFeedback、CleanupNodeQueue 等无 Wait 字样的 scope 也可能调用同步API；先读源码，不能把名字分类当 on-CPU。`ActivateAwakeRecursively` 还会出现在 Deactivate 下，按完整路径解释。
- `PxScene.simulate` 也可能包含帮助执行任务和等待完成的循环；高 self 不能自动解释成大量碰撞计算。
- GPU大类/root长区间可能含同步或缺插桩。没有具体pass/queue/submission证据时写“GPU侧待归因”，不编造shader根因。
- Thread name 不是唯一身份，使用 owner_ref/path；FrameSet ref/index 从本轮结果读取，不硬编码索引1。GPU原生时间片不能伪装成Player.Frame。
- 记录实际stack depth；0也能通过现有作用域插桩发现问题。没有受控A/B不量化采集总开销。请求启用的sampling/context-switch不等于落盘有数据。
- 标准 Query frame.outliers 在当前版本会把推断尾帧算完整；本分支报告统一引用 triage 完整帧分母/分位，不能混用。

源码读取遵循 [evidence-and-source.md](evidence-and-source.md) 的版本/快照规范。用户提供的仓库路径可直接作为授权源码根；不被旧本机示例路径阻挡。源码版本未匹配时解释当前机制和可验证的优化方向，不声称捕获时实现完全一致。

## 输出与验收

保留 `分析过程记录.md`、命令与策略、原生manifest/候选/窗口/Query回执、audit、源码快照、问题/反证/待补证。输出 `Typora/Performance-Analysis-Report.md`，详细观察每册至多5项、按实际主要帧序排列；表头固定明确到 CPU/GPU→线程→函数→耗时→动作，等待项指出真正完成端是否已找到。

`stream-analysis-audit/v1` 是独立保存输出核验，**不冒充旧Schema4/candidate-policy-v3/validate_analysis_result已通过**。展示按 [native-profiler-views.md](native-profiler-views.md)，无原生定位/图片/交互证据时就地记录degraded，不伪造链接。无需为展示失败重扫已验证的数据。

本分支能给出全量可观测候选和可审计函数线索，仍需人工解释源码、依赖和优化方案；不能宣称全自动根因诊断。现有历史普通.tracy流程保留，不静默改变其schema或审批含义。

2.6.0：stream 逐问题原生预览和网页内嵌交互按 [stream-native-views.md](stream-native-views.md) 执行；每项观察和 P0/P1 行均需真实帧/事件入口，不能只给图库或跳到桌面。
