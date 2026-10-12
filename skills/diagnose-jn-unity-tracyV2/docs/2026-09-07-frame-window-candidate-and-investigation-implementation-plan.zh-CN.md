# Query 原生帧/窗口候选与 AI 调查闭环实施计划

日期：2026-09-07。状态：已获用户批准，实施中。本文承接 2026-09-06 Query-native deterministic scan 计划；下列新规则覆盖旧版的全局 Top 截断和仅 P1 必查规则。

## 当前接续基线（2026-09-09）

下文第 2/6 节的旧 Session 衍生工作树仅为历史记录，不再用于本轮开发。

- 当前工作树：`C:\CodeProjects\GodotProjects\tracy-0.13.1-ai-query-scan-dev`。
- 当前分支：`feature/JNTracy-AI-QueryScan-DevBase`。
- 已按用户授权 fast-forward 合入 `feature/JNTracy-QueryAnalysisCache`，HEAD `80bceb339014fc02f22a18945fd510bfc14ccf65`；不带 Session Store。
- 当前 Skill 源码：此工作树的 `skills/diagnose-jn-unity-tracy`；Query 构建：`build-query-devbase`。
- 缓存分支已完成同一短 Trace 的完整统计、候选、全部分页和自然热点检出。FW5 不再卡在旧 run2 的 16 GiB 精确统计失败点。
- 本轮证据：`C:\Users\Admin\Documents\JN-Unity-T3\QSCAN15-Cache-Merge-and-FW-Acceptance-20260909`；继续 FW4/FW6，不复跑 30 分钟 Session。
- 生成旧缓存的冻结 EXE 与重建 EXE 分别记录身份；不跨 EXE SHA 改写缓存身份。
- 所有 selected 的读取契约通过不等于根因调查完成；2,596 个必查项仍须真实调查。安装版 Skill 尚未更新。

合并核验与接续边界见仓库 `doc/JN-Tracy-Skill-Cache-Integration-20260909.zh-CN.md`。

## 1. 目标与范围

解决两类已复现问题：局部高峰、稳定偏慢的具体事件被全局统计或名额挤掉；入选 P2 仅进入待办而报告仍宣布完成。保留全局统计的价值，不以代表帧代替全量扫描。

数据流程：

```text
完整 Trace → Query 正确统计
                  ├─ 每帧候选
                  ├─ 连续窗口候选
                  └─ 全局候选
                         ↓ 合并、去重，保留来源和不同表现
                   selected 候选调查账本
                         ↓ 定向 MCP、树展开、等待链、源码/反证
                   完整报告或明确阶段报告

源 Capture 质量限制 → 独立附录；只在某性能结论依赖缺失数据时补查
```

仅修改 Tracy Query/analysis、Skill 草稿、验证脚本和报告。最后将通过验证的草稿同步到安装版，备份安装版并迁移机器配置。不改 Unity、PackageRepo、Player，不重新录制，不做 30 分钟 Session 验收，不自动合并或推送 dev。

## 2. 基线与产物

- Query 仓库：`C:\CodeProjects\GodotProjects\tracy-0.13.1-ai-query-scan`。
- 分支：`feature/JNTracy-AI-QueryScan`；基线 HEAD `fd4388533e1c6b31074dfa1c46aa3d73ff1ec591`。
- 保留工作区已有 Quality Appendix 改动；不覆盖其他任务工作。
- Skill 草稿：`C:\Users\Admin\Documents\JN-Unity-T3\skill-draft\diagnose-jn-unity-tracy`。
- 安装版：`C:\Users\Admin\.codex\skills\diagnose-jn-unity-tracy`。
- 唯一真实验收输入：`C:\Users\Admin\Documents\JN-Unity-T3\QSCAN13-Short-Skill-Acceptance-20260907\oracle-existing\GPUCatalogOff-HighEvidence-20260903-202313.tracy`。
- Trace SHA-256：`da5495e1bc5d85962bb0f9bdb2cabdc41e35c51ea0f1576a9eab702f36c15211`。
- 新证据独立存入 `QSCAN14-Frame-Window-Investigation-20260907`；旧报告、扫描和手工对照证据保持原样。

## 3. 已锁定的十二项规则

### 3.1 树状 Zone：全层扫描，按可解释成本选取

遍历所有已录层级。保留每次耗时、每帧累计耗时、调用次数、inclusive、直接子区间并集和 residual/self wall。父节点大部分由子节点解释时向具体子节点下钻；父节点剩余时间大时形成“未解释工作/等待”候选；大量低耗时调用形成累计成本候选。等待候选查完成端。不同线程/Queue 不相加，父子 inclusive 不重复作为总贡献。Residual wall 不等于精确 on-CPU。

### 3.2 预算与排名各司其职

60 FPS 帧预算采用 16.666667 ms。历史 Lua/Render/Animation 等模块预算默认只作参考，不独立制造必查候选。GPU 容量 6.4 GB 为明确阈值，CPU 16 GB 为暂定阈值，十进制 GB，物理/逻辑/引用口径分开。候选由实际成本、局部排名、变化、有效阻塞关系产生，不必先通过全局 P95。

### 3.3 可配置初始门槛

每个问题帧/窗口按 FrameSet、线程或真实 GPU Queue、指标分别取 Top 5，最低 0.2 ms；实际工作或未解释子树帧累计达到 1 ms 可独立入选；相对增长同时要求至少 0.5 ms 且至少 50%。已证明阻塞关系和用户关注绕过最低成本/Top 限制。父包装层不占满具体子节点名额。基线为零/未知时不构造无限倍率，走绝对成本或 unavailable。

这些是初始检出参数，不是测得的自然常数；放入唯一 YAML Profile，经 Query 规范化并进入配置身份。只能有证据地校准，不能为缩减工作量静默提高。

### 3.4 连续窗口

30/60/120 帧滑窗；有效帧比例和超预算比例分别记录。有效帧中至少 80% 超预算且中位数超预算为连续压力；其中 MAD/median ≤0.1 标为稳定偏慢。低有效覆盖（默认不足 80%）不宣称完整稳定窗口。重叠窗口合并并保留覆盖区间、尺度、起止/峰值/恢复证据。小于窗口的头尾仍参加逐帧筛查。不把统计窗口命名为战斗、加载等业务阶段。

### 3.5 参照窗口

优先相邻同 FrameSet、完整性足够的低负载窗口；返回选择理由、预算、离散程度、时间距离及结构可比性。场景/相机未知明确记录。无可信参照时保留绝对耗时/超预算候选，不制造相对结论。290 与 540 附近只能建立同一次捕获内的观察比较，不宣称受控 A/B 因果。

### 3.6 候选身份

身份含 domain、FrameSet、thread/queue、稳定调用路径、指标和表现类型。高峰与稳定压力可共享 family，但必须各自有调查覆盖；同名不同线程/路径不能合并。局部、窗口、全局三路独立产生，再取并集。局部/窗口已入选项不能被全局 Top、per-domain cap 或时间预算剔除。分页/批次只控制处理规模，不充当隐形总上限。

### 3.7 代表帧

按表现保留最坏、典型坏、有效正常/较轻参照；窗口补充开始、峰值、结束、恢复及稳定区间多个位置。同一帧可兼任角色，不同表现的证据要求不被去重抹掉。不设最终 12 帧总上限。报告统计来自全量 Query，不来自这些抽样帧。

### 3.8 GPU 边界

不要求 Unity 新增 OriginFrameId。不具备 CPU-origin Frame 关系时仍基于已校准的物理 Queue 时间窗和完整 L0 片段产生 GPU 候选。保留正常渲染和 UI 的两个 L0；时间重叠为观察关系，不能伪造 Player.Frame 的 GPU 总耗时或跨 Queue 嵌套。

### 3.9 统计正确性修复

invalid/unknown 帧值不补零；已知没有发生的调用才允许零。同名嵌套区间并集与调用总和分开。多个 FrameSet 不混入同一排名分母。Job execution 不当作 critical path，只有 continuation 不证明帧阻塞。GPU physical peak 使用同时存活分配，不使用历史创建字节总和。不能证明的指标返回 unavailable，而非输出可疑数值制造候选。

### 3.10 调查完成门禁

所有 `selected=true` 都必须调查；优先级仅决定先后。每项要有真实 MCP 收据：method、params、Trace/scan identity、请求与响应路径/SHA、成功或明确不可用状态，并能对应候选的 Frame/线程/实体及表现。包装层需展开；Wait 需查询完成关系或保存实际 capability-unavailable 回执。使用源码时记录 revision 和内容身份。

仅填 analysis_trail、复制候选数值、写“已查询”不能通过。结论允许 Confirmed/Supported/Hypothesis/Rejected/EvidenceGap 等证据层次；机器只保证调查动作、覆盖和引用可审计，不能保证人类因果判断自动正确。复用调查要指出覆盖哪些候选及哪些表现。

### 3.11 恢复与报告

记录 selected 总数、已调查、已调查但证据受限、未调查；任何 selected 未调查只能生成阶段报告。时间/内存限制保存队列和已验证证据，恢复不重做已完成项。保留原始峰值；典型运行统计是额外视图，不替代全数据。没有有效不重叠分母不能声称“解释 80% 整帧”；CPU 各线程和 GPU 各 Queue 分别报告未解释部分。

### 3.12 验收与发布

新统计/候选算法更换 cache identity，旧结果不可冒充新算法，不删除旧缓存。以短 legacy Trace 验收；Session 支持不等于本轮验收。功能/测试通过后同步安装版及脚本，保留机器配置并明确 JSON→YAML 迁移；禁止安装版与草稿版永久使用不同规则。

## 4. 实施任务与文件映射

| 阶段 | 工作与主要文件 | 退出门禁 |
|---|---|---|
| FW0 基线/计划 | 本文、三路规则、证据目录；记录 dirty 和工具身份 | 输入/授权明确，旧数据保留 |
| FW1 调查完成门禁 | `manage-analysis-state.py`、`query_native_report.py`、结果 schema、模板、tests | selected P2 未调查拒绝完整报告；阶段状态可保存 |
| FW2 正确统计 | `TracyFrameCpuScanner`、`TracyExactStatistics`、`TracyAnalysisScanManager` | unknown 非零、路径身份、FrameSet 隔离、Job/Memory/GPU 正确性测试 |
| FW3 帧/窗口选择 | 新独立 local policy 模块、`TracyCandidatePolicy`、Profile/schema | 三路不相互挤出；稳定慢/孤立峰/双峰/低成本累计测试 |
| FW4 MCP/证据契约 | Scan 序列化、manifest schema、报告验证器、Skill refs | 真实回执、候选范围和不同表现关联可检验；缺失引用被拒绝 |
| FW5 合并构建与短 Trace | incremental Query build、常驻 MCP、新 scan/report | 396/397、290/400/540 的已知遗漏自动检出；无名称/帧号硬编码 |
| FW6 完整 AI 调查/安装 | 所有 selected 调查、报告、配置迁移、安装备份 | 完整报告门禁通过、质量独立附录、安装版测试通过 |

每阶段记录测试命令、失败→通过、源码身份和已知限制。主要真实扫描在轻量回归通过后集中执行，不每改一处重跑全量。阶段失败不宣布通过；若达到当前交互资源限制，留下明确阶段报告和下一项工作。

## 5. 测试驱动清单

- 同一热点放在长正常捕获中，其局部表现不因稀释消失。
- 新增大量更大全局热点，局部/窗口已选集合仍保留。
- 连续 24 ms 帧（MAD 近零）必须成为稳定偏慢窗口。
- 单个大峰、两个离散峰、连续大峰、低成本高频累计均可检出。
- 大父包装下的小数量具体重子节点不被父节点占位。
- 同名不同路径/线程、不同 FrameSet 不能串合。
- unknown/invalid 不补零、Wait 残余不等于 on-CPU、Job 非 critical path、显存生命周期峰值。
- GPU 无 OriginFrameId 仍检出真实 Queue 片段压力；不得伪造 CPU Frame 关联。
- 用户 focus 绕过阈值，但不绕过数据可信度。
- selected P2 留 Backlog 时完整报告失败；伪造/缺失/错 Frame 回执失败。
- 调查受限要有实际查询失败/缺失原因，可作为已调查但受限；未经查询不能伪装受限完成。
- Quality 不进入性能 P0/P1/Top，不占名额；scanner 自身完整性失败仍阻断。
- Skill 行为测试：时间压力下仍不能把未调查 selected 放入 Backlog 后称为完整。

## 6. 执行与证据

Query 增量构建使用仓库 `build-query-scan`，执行 CandidatePolicy、AnalysisScanManager、ExactStatistics、FrameCpuScanner 和新增 local policy 测试。Skill 执行 Python unittest、PowerShell script/distribution tests；依赖缺失的检查单独记录，不虚称通过。

真实验收通过一个常驻 `tracy-query --mcp`：记录 initialize/tools/schema、trace identity、profile_validate、scan status/summary/quality/candidates 完整分页、candidate_get/representative_frames，再对所有 selected 深查。Python 仅解析配置、维护状态、校验/渲染 Query 结果，不从 Trace 扫描候选。

报告至少包括：范围/身份、总体帧分布、帧/窗口/全局候选对照、每个表现的调查过程、支持与反证、源码匹配、可执行优化方向、未完成调查、证据限制、独立质量附录。单 Trace 不推断 Tracy 总体 A/B 开销。

## 7. 回退与完成定义

只回退本任务新增代码，保留旧 Quality Appendix 改动、原始 Trace、历史报告和机器配置。默认不执行 commit/push/分支切换；如需发布 Git 提交另记录范围。安装前完整备份受影响文件并保存 SHA-256，可按清单恢复。

本计划只有 FW0–FW6 均通过才算完整完成。代码或 synthetic tests 通过不等于短 Trace 全流程验收；扫描完成不等于 AI 调查完成；报告生成不等于结论全部已证实。

## 8. FW5 run2 后补充的实施门禁

run2 在三个域扫描后进入精确统计，触发 16 GiB 保护并安全取消。临时注册表有 1,506,514 个签名组（不是 selected 候选数）。已用独立红绿测试修正每个指标把 32 槽临时异常空间保留到长期结果的问题，统计结果 hash 不变。

在第三次真实全量扫描前，先完成高基数合成回归：长期 Aggregate/Context/排名/代表项、hash、publish、缓存读取不得因整体 JSON 对象和字符串复制放大而失控。需要分页/分块时保留全部精确事实与候选语义；不提高门槛、不减少数据域、不改用 N30 Session。取消与正式中间结果复用必须有身份、字典和 checksum，不凭未完成 `.work` 文件伪装恢复。

此为 FW2/FW5 已批准内存/取消边界的落实，不改变十二项业务规则。当前真实验收失败仍阻止 FW6 安装发布；详见 `QSCAN14-Frame-Window-Investigation-20260907/FW5-Run2-Exact-Statistics-Memory.zh-CN.md`。

## 9. 2026-09-09 缓存合并后的实际位置

上节是历史失败记录。缓存分支已在独立 dev-base Skill 工作树合入 `80bceb33`，短 Trace 的完整精确统计/候选已完成并可复用，不再重做失败全量扫描。没有引入 N30 Session。当前续接的是 FW4/FW6，而不是重新执行 30 分钟验收。

新增流式候选 reader 接通报告、数值校验和状态审计；真实 1,508,732 候选在 43.875 秒内完成身份/Backlog 校验，100 ms private 峰值采样 811,458,560 B，全部 2,596 个未调查 selected 仍拒绝 complete。此测试不替代完整报告渲染/全链内存门禁。

真实 MCP 树缺少祖先时改为验证独立父链回执；已用 6 个自然候选所需代表范围验证，并展开 Sampling/CS/源码假设与反证。Python 73/73、PowerShell/distribution/quick_validate 通过。额外 linter 缺依赖单列为未完成。全部 selected 调查、最终报告和安装仍未通过；不能把 6 项回执验证写成已完成全量诊断。

完整进度和证据见仓库 `doc/JN-Tracy-Skill-Cache-Integration-20260909.zh-CN.md` 与本地 `QSCAN15-Cache-Merge-and-FW-Acceptance-20260909/Plan-and-Progress.zh-CN.md`。仍按原完成门禁执行，不增加性能阈值、不削减候选、不自动提交或覆盖安装版。

## 10. 续接进展：阶段报告与实际 GPU 回执

实际阶段报告已生成两次，3,390个文件字节一致；约45秒/次，private观测峰值约3.40GB。1,508,732候选完整保留，只有六项CPU已进入Findings（1 Supported、5 Unresolved），不是完整性能报告。独立恢复状态已经实际审计并落盘，2590个selected仍待查。

FW4补齐真实GPU DTO的CPU线程/GPU Context区分及ref-based L0区间锚点；两项自然GPU候选的四个代表片段、八个实际匹配实例父链校验通过，没有虚构PlayerFrame。最终Python78/78，PowerShell/distribution通过。

Job定向深查暴露独立Query问题：全局GetJobs展开导致内存保护，domains=[frame,job]替代路径仍超时。该候选未完成，不伪造source unavailable。后端范围读取修复尚未实施，定位及下一步范围见本地 `FW4-Job-Directed-Query-Blocker.zh-CN.md`。FW6完整调查与安装仍不放行。

## 11. 2026-09-09 Job 修复后的真实 SKILL 续验

上节Job阻断为历史状态。范围读取修复已完成并通过25/25原生回归；继续实际MCP调查9个OriginFrameId、27个Job。修正收据中OriginFrameId与Player.Frame数组下标混淆，独立核验canonical帧身份，并在正式Schema增加 `job_frame_identity_evidence_id`。

真实Job收据通过；Python81/81、PowerShell总测试和隔离distribution通过。七项阶段报告两次生成，3738文件完全相同，约49秒/次、private峰值约3.51GB。附加quick_validate因当前Python缺yaml单列未完成，不改变已完成的测试结果。

本轮报告新增Shader创建相关Worker长尾与真实依赖组等待的证据，也保留Native符号不足和单帧critical-path-cycle限制。Quality不成为性能P0；SiteReuse不冒充执行栈；CPU搜索的返回线程逐ref核对。当前7/2596 selected已实际调查，2589仍待查，报告in_progress，FW6及安装未放行。没有新scan、Session或Unity/Player变更。

证据：`QSCAN15-Cache-Merge-and-FW-Acceptance-20260909/FW4-Job-Skill-Investigation-Acceptance.zh-CN.md`、`job-actual-receipt-bindings.json`、`job-stage-report-verified/determinism.json`。

## 12. 2026-09-10：已确认的 FrameSet 缺失边界与状态修复

用户已确认：FrameSet duration 候选在代表区间确实没有 CPU Zone 时，可以完成该区间的证据调查，但只能给出 Unresolved 和最小缺口。已实现 `verified_absent`：严格 frame-wall 身份、真实 frame.get、原始无过滤 CPU search 首页、精确边界、完整空结果、零遗漏、无预算耗尽。请求可使用候选的 FrameSet 名称，但响应稳定 ID 必须精确匹配；不能把名称当成 opaque ID。

普通 CPU/GPU Zone 不适用该分支；有记录的代表区间仍需真实树。错误、部分结果、未读页、筛选后空集、children=[] 以及 Supported/Confirmed 均不能通过。顶层 EvidenceGap 必须绑定具体候选与搜索/Frame 回执，不能只写泛化“数据不足”。

报告校验输出统一增加 report_status、analysis_complete、required_pending。passed 只表示产物合法；完整分析要求 complete/true/0。没有放宽 selected、数值一致性、质量附录或状态恢复门禁。

91 项 Python 测试通过；PowerShell、分发、quick_validate 通过，linter 0 error（7 项既有 warning 保留，不擅改许可证/CI）。独立真实回执行为复测 21/21 通过。没有重新扫描，没有启动 Session，没有改 Query EXE、Unity、Player 或采集配置。

真实调查累计 32/2596；本批队列剩余21项P1已全部处理，加上此前2项，累计23项P1均已调查。2564 项 selected 仍待查。Unresolved 不等于已证明根因。FW4 此次修复已验，FW6 完整分析与安装仍未完成，禁止覆盖安装版。最新证据见本地 `FW4-Verified-Absence-and-Report-Status-Acceptance.zh-CN.md`。

最终32项阶段报告两次生成，9176文件字节一致；76.718/75.719秒，private采样峰值3839000576/3839623168 bytes，均低于4GiB保护线。恢复审计76.063秒，完整保留1508732候选及2564待查项；未重算Trace统计。最终机器状态为passed=true、report_status=in_progress、analysis_complete=false、required_pending=2564。
