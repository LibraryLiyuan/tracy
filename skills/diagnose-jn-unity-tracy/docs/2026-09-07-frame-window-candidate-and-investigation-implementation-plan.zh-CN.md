# Query 原生帧/窗口候选与 AI 调查闭环实施计划

日期：2026-09-07。状态：已获用户批准，实施中。本文承接 2026-09-06 Query-native deterministic scan 计划；下列新规则覆盖旧版的全局 Top 截断和仅 P1 必查规则。

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
