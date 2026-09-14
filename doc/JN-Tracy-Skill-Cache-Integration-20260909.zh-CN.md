# Query 缓存合并与 SKILL 调查流程续接

2026-09-09。本文件记录当日已实施内容及剩余门禁，不是完整 AI 分析验收声明。

> 历史记录：下文二进制身份、测试数量、“安装版未发布”和 P 级均为当时状态。当前 SKILL 已更新为 2.2.0；其现行优先级与完成要求见仓库 skills/diagnose-jn-unity-tracy/SKILL.md。

## 1. 合并

- 目标：`feature/JNTracy-AI-QueryScan-DevBase`，工作树 `C:\CodeProjects\GodotProjects\tracy-0.13.1-ai-query-scan-dev`。
- 来源：`feature/JNTracy-QueryAnalysisCache`。
- `0cb512521481501f1aeff72af5a73b445a26dad2` → `80bceb339014fc02f22a18945fd510bfc14ccf65`，`--ff-only` 成功，无冲突。
- 共 60 个来源文件变更：analysis、Query、CMake、测试和文档。没有引入 N30 Session，不修改 dev/N29/N30，不推送。
- 目标原有 `build-devbase-deps/` 保留；安装版 Skill、原始 Trace 和历史缓存没有覆盖。

## 2. 构建与二进制身份

新编译 Query：

`build-query-devbase/Release/tracy-query.exe`

SHA-256：`5f039b6d5d92e9a125d29af775214dc067ea7af760fb52713e3b6603744568fc`。

已完成真实缓存的冻结 Query：

`C:\CodeProjects\GodotProjects\tracy-0.13.1-query-analysis-cache\build-query-analysis-cache\ac6-gpu-local-quality-20260909\query-bin\tracy-query.exe`

SHA-256：`f62aed6c82f93340620f44e936fc1cc09f710b0134d1bc5dd464716d1a227540`。

59 个冻结源码/测试文件与合并后目标文件逐项校验，区别只允许 Git LF/CRLF 换行转换，原始与目标 SHA 分别保存。冻结来源文件本身仍须与原验收 SHA 完全匹配。不能把换行归一化相等写成原始文件 SHA 相等。

新 EXE 正确拒绝旧 EXE 的 scan identity；旧 EXE 按原身份读取缓存成功。没有手改 manifest、EXE hash 或 algorithm ID，没有因此重新执行全量扫描。

配置保留了第三方依赖的原有 CMake dirty-cache/可选依赖/LTCG 警告。初次 CTest 提前于最后链接完成，一项 MCP transcript 测试因文件被链接器占用而未启动；构建结束后整套重跑，不把未执行项目算作通过。最终 CTest **24/24 通过，138.63 秒**，见独立证据目录的 `ctest-final.log`。Skill Python 62/62、缓存 MCP 客户端 10/10、PowerShell、distribution 和 linter 均通过。

## 3. 常驻 MCP 核验

输入仍为短 `.tracy`：

`C:\Users\Admin\Documents\JN-Unity-T3\QSCAN13-Short-Skill-Acceptance-20260907\oracle-existing\GPUCatalogOff-HighEvidence-20260903-202313.tracy`

Trace SHA：`da5495e1bc5d85962bb0f9bdb2cabdc41e35c51ea0f1576a9eab702f36c15211`。

- 新 EXE：Query 1.35、无 Session 方法、Profile 规范化一致；原始短 Trace 加载成功，290/400/540 的 Frame 数值与 Oracle 完全一致。约 9.66 秒，进程 private 观测峰值 5,886,230,528 B。
- 冻结 EXE：无需打开 Trace/Worker，summary/quality、首中末页、单候选/代表帧和六个自然热点候选核对通过。约 2.25 秒，private 观测峰值 21,643,264 B。
- 两个会话均未调用 scan start/resume，退出码 0，原 scan-state SHA 未变化。新会话没有重复先前 75 分钟源扫描和 8 分钟全量冷读验收。

既有真实验收：1,506,513 组统计、1,508,732 个候选、2,596 selected（P1 23、P2 2,573）。来源 GPU 局部质量限制保留；完整 L0 子集不能代表捕获整体 GPU 成本。

## 4. 本轮继续实施的 SKILL 修正

### 4.1 候选导入

原导入器先 `list(pages)`，再保留所有完整 Candidate，CLI 也先解析全部 JSON。完整候选导出约 3 GB，可能在 Python 再次产生全表放大。

修正为逐页校验、只保留精简调查元数据，完整数值/代表区间留在原始 MCP 页。所有未选项仍保留 Backlog，全部 selected/P1/UserFocus 进入队列；不减少统计域、不调高门槛、不重新排名。

新增红绿测试同时复现并修正：cursor 跳号、提前 done、游标不推进、partial 页被误接受。晚到重复 ID/不完整分页不能部分替换原状态。

真实导入：3018 页、1,508,732 条记录，完整逐行 Candidate hash 与缓存分支全量分页哈希一致；2,596 必查、已调查 0，完整报告状态被拒绝。耗时 96.375 秒，采样 private 峰值 1,944,371,200 B。状态模型仍按候选数量增长，不宣称 Skill 全部内存常量化。

### 4.2 状态写盘

旧 `_atomic_write` 同时保留完整 JSON 字符串和 UTF-8 副本。15,000 条元数据夹具产生 15,918,800 B 临时分配，超过 1 MiB 测试门禁。改为流式 JSON 编码，保留 flush/fsync、atomic replace、排序/缩进/UTF-8 字节完全相同；新测试通过。

上述真实导入的 96.375 秒结果先于此编码修正，使用验收客户端流式输出，不冒充最终生产 CLI 的全链实测；生产写盘由独立内存与字节一致性回归覆盖。

### 4.3 参考文档

- 更新 `native-scan-v3` 与 EXE-bound cache 恢复规则。
- 首页 cursor 明确为省略或 `"0"`，不再指引显式空字符串。
- GPU 使用 `candidate_get.representative_intervals`，不把 `representative_frames=[]` 当成无证据；保留 `when_present` 与完整 L0 分母边界。
- 本轮生产/回归代码在当前分支；安装版未发布。

## 5. 当前阶段与下一步

| 阶段 | 当前边界 |
|---|---|
| FW0/FW1 | 已有基线和 selected 完成门禁保留；本轮继续验证 |
| FW2/FW3 | 缓存分支真实全量统计/三路候选通过；GPU 源质量不是全部 complete |
| FW5 | 已有短 Trace 完整缓存证据，本轮合并/重建/定向 MCP/队列导入续接；不再停在旧内存失败 |
| FW4 | 逐项调查收据与不同数据域的实际契约仍需持续验收；本轮修正导入/分页/状态写盘 |
| FW6 | 未完成全部 selected 的根因调查、完整报告和安装发布 |

后续按原计划：先使报告验证/渲染端按页或索引读取候选，不能把 3 GB 全量候选再次整体 JSON 加载；保留完整 Backlog 与身份审计。随后按队列开展真实 MCP 树展开、等待完成端、反证和源码核对，允许有审计绑定的共同机制分组，但每个表现都必须有实际查询覆盖。不能用“已点查”代替根因调查，不能把未调查改名 EvidenceGap。

缓存分支的 2 GiB 保守预留软目标仍未达成（历史合成 2.70 GiB，4 GiB 硬预算通过）。本轮不调低安全系数或放宽 Profile，不将这一离线工具指标混入游戏性能结论。

## 6. 证据位置

`C:\Users\Admin\Documents\JN-Unity-T3\QSCAN15-Cache-Merge-and-FW-Acceptance-20260909`

包含 configure/build、CTest 首轮/最终轮、MCP 原始请求/响应、源码与 EXE 身份、红绿/62 项 Python 回归、10 项 MCP 客户端回归、PowerShell/distribution/linter、真实候选页账本及状态。

安装门禁未完成；本报告不宣布完整 SKILL 发布。

## 7. 继续执行：FW4/FW6 报告读取与真实回执

### 7.1 已实施

- 新增 `scripts/candidate_manifest.py`：小型 manifest + SHA/count 绑定的 NDJSON；小型内嵌候选仍可读。逐记录验证，EOF 校验完整 count/hash。
- report validator、numeric validator、artifact audit 使用同一个 reader。保留完整 ID/必查集合；仅为实际 findings 保存完整候选 payload。
- 报告复制保留原始 manifest/NDJSON 字节，避免全量 payload 重序列化。保护相对路径、保留文件名和 Evidence 输出冲突。
- 缺失 manifest、尾部损坏统一走验证失败，不能发布或留下成功 manifest；补齐独立测试加载与错误输入覆盖。
- `zone.tree` 的独立 `zone.get` 祖先回执逐项验证；FrameSet 根使用严格 typed identity + 实际 frame.get 锚点。不伪造原始响应。
- 文档加入局部 Sampling 的真实方法边界、完整 opaque callstack ref、SiteReuse 与执行热点的区别，以及相交 CS 区间不能直接全额归入窗口的限制。

### 7.2 本轮回归与实测

| 测试 | 结果 | 边界 |
|---|---|---|
| Python Skill | 73/73，5.178 秒 | 含新增报告读取与实际收据红绿回归 |
| PowerShell / distribution | 通过 | 不覆盖安装版 |
| quick_validate | UTF-8 模式通过 | 初次系统默认 GBK 读取失败，保留日志后显式指定 UTF-8 |
| 额外 agent-skill-linter | 未完成，缺 click | 未安装依赖，不宣称此检查通过 |
| git diff --check | 通过 | CRLF 提示不是 whitespace 失败 |
| 真实候选流式验证 | 1,508,732 项；43.875 秒；811,458,560 B private 峰值采样 | 100 ms 采样、仅身份/Backlog 校验；不是最终报告全部路径峰值 |
| 必查完成门禁 | 全部 2,596 个未调查项拒绝 complete | 没有删项/调阈值/只读前 N 项 |
| 六项自然候选回执 | 所需代表范围/父链/线程/hash 校验通过 | 不等于根因已调查完毕 |
| Sampling 定向批次 | 10.281 秒；private 峰值采样 5,889,662,976 B | 同一个短 Trace，无 scan start |
| 补充分页/准确栈引用 | 9.953 秒；private 峰值采样 5,899,341,824 B | 400/540 HDWorlds 树完整分页 |

早先 report-stream-real 的 519,999,488 B 只在若干点采样，不作为全程峰值；以上以补充的 100 ms 监测结果为准。所有候选元数据、Backlog 和最终 Markdown 渲染仍可能随候选数量增长，不宣称整个 Skill 常数内存化。

### 7.3 真实调查进展与剩余工作

OwnedUsage 高峰已在严格 Zone/主线程时间内观察到 Skinned LOD Synchronize→CollectGroups/SelectGroup；540 帧该事件很短，是不能用它解释全部稳定慢帧的反证。DrawDispatchGroup 的长墙钟包含执行与让出/等待迹象，不能按名字当成单个 Draw/GPU 耗时；精确等待生产者仍未证实。当前代码仅供机制假设，Capture Git revision 缺失，不升级源码结论为 Confirmed。

详细的逐步查询、数值、源码 SHA 与人工 GUI 对照在：

`C:/Users/Admin/Documents/JN-Unity-T3/QSCAN15-Cache-Merge-and-FW-Acceptance-20260909/Directed-Investigation-Stage-Notes.zh-CN.md`。

下一步仍是 FW4/FW6：按不同 manifestation 补齐剩余域/候选的实际调查，保存结构化 Findings 与审计绑定，完成完整报告两次确定性生成及全链内存验证，再做安装验收。不得把本轮 6 项回执结构验证替代所有 2,596 项必查根因调查；原队列未改为完成。

新代码继续保存在 `feature/JNTracy-AI-QueryScan-DevBase` 工作区，未提交/推送；不修改 dev、Session、Unity 或 Player。

## 8. 真实阶段报告、GPU 回执与 Job 深查阻断

### 8.1 报告已经实际生成，不再只是导入/校验

`QSCAN15-Cache-Merge-and-FW-Acceptance-20260909/generated-stage-report-3/` 和 `generated-stage-report-4/` 是独立的两次实际生成。

- 1,508,732 个候选保留；六个 CPU findings 实际组装，1 Supported、5 Unresolved；不是六个根因均已证实。
- 2,596 个必查中仅 6 个进入本阶段 Findings；剩余必查项不隐藏、不提升/降低优先级。
- 全部 3,390 个产物逐字节一致，没有排除时间/路径文件。两次渲染分别 45.734/45.156 秒，100 ms private 峰值 3,401,576,448/3,402,113,024 B；4 GiB 保护未触发。
- 结构化组装 23.516 秒，682,516,480 B。不是整个 Query 进程的内存数据。
- `report_status=in_progress`，`complete_skill_acceptance=false`；完整候选和 Backlog 保留。机器校验通过不代表完整 AI 结论验收通过。
- 补齐此前滞后的正式 JSON Schema：阶段状态、实际 investigation_receipts、interval-only GPU 候选、原始 MCP envelope。真实 1,686 条 evidence manifest 通过 PowerShell Test-Json；整个 416 MB analysis-result 由生产 validator 校验，未宣称其全部通过 Test-Json。

证据：`full-stage-report-schema-verified/`、`stage-analysis-result.json`、`stage-evidence-manifest.json`。第一次修正前的两次渲染仍保留在 `full-stage-report-measurements/` 和 report-1/2。

### 8.2 GPU 实际路径与回执修正

初次 GPU root 查询只拿到 L0/直接子节点，不能覆盖候选 Pass。随后 `gpu-exact-paths/` 按自然候选路径完整展开四个代表 L0，共八个匹配叶/Pass 实例，14.562 秒，private 峰值 5,886,808,064 B。没有按照名称对不同父路径合并。

GPU 真实 DTO 同时含 CPU thread_ref 和 GPU context_ref，旧 validator 优先 thread_ref 导致误拒；ref-based GPU tree 也缺少真实 L0 锚点通道。先添加失败测试，再修正 Queue 选择和 interval_anchor 的 ref/时间/祖先校验。其他 Queue、其他 L0、缺少 GPU 时间、区间外事件、partial 及错误 ref 均拒绝。

`gpu-parent-receipts/` 补取真实祖先 get、SiteReuse callstack 和 MegaLight PostRayTrace 子树，9.750 秒，private 峰值 5,887,049,728 B。两项自然 GPU 候选的真实 receipt 验证无错误。这里只通过动作/范围审计；这两项未加入六项 CPU 阶段报告，不计作完整根因调查。GPU 执行瓶颈、资源关系、完整 Player GPU frame 仍有各自证据限制。

最终 Python 回归 **78/78，8.730 秒**，见 `skill-gpu-receipts-final-tests.log`。当前生产修改仍只在 Skill 文件中，未改 Query C++ 或冻结 EXE/cache identity。

### 8.3 Job 深查失败不等于缺少 Job 数据

`timeline.correlated_slice` 查询一个明确 JN OriginFrameId 时，全局 GetJobs/关系展开先于帧过滤；private 观测达到 21,043,539,968 B，16 GiB 外部守护终止本任务自己的进程。采样守护存在越界后才观测的超调，不能宣称该过程始终低于16 GiB。

换用 `evidence.graph`、domains=[frame,job] 后，private 峰值 9,356,070,912 B，没有触发内存保护，但180秒请求上限内无响应；含加载和关闭总198.484秒。代码显示 domains 主要过滤图节点，多项未请求数据仍被读取；具体超时热点尚未做函数级测量，不宣称已证明全部耗时来自某一个 getter。

两次失败均未形成 Job 的合法 unavailable 收据，不把候选标为已调查/完成，也不混入游戏 P1/P2。未重复同一高成本请求，未提高内存/时间上限。详情和最小后续改动建议见本地 `FW4-Job-Directed-Query-Blocker.zh-CN.md`。

### 8.4 恢复点

FW4：CPU真实范围及GPU回执已验证；Job定向接口仍阻断。FW6：阶段报告生成/确定性门禁通过；全部 selected 调查及安装未完成。接下来应先处理定向 Job 读取范围，再恢复候选队列，不重新做完整扫描。旧持续目标是 paused 且含已暂缓 Session 验收；本轮没有恢复这个过时目标，不改变最新的短 Trace 范围。

独立恢复状态 `QSCAN15.../audited-stage-state/analysis-state.json` 已保存：生产审计核验六项原始调查文件/回执，再读取原始3018页候选恢复完整队列，required=2596、investigated=6、pending=2590，task仍investigating_candidates。71.141秒完成；页边界private峰值采样1,942,687,744B不代表整个审计/写盘峰值。原状态未覆盖，新增GPU两项不冒充根因完成。PowerShell与distribution本轮再次通过；git diff --check通过（只有CRLF提示）。

## 9. 经用户授权的 Job 定向查询修复

此前8.3/8.4中的阻断是历史状态。本轮开始修改Query C++，不再是仅修改Skill；没有变更扫描算法、缓存身份、Session或Unity。

- 新增 `JobEvidenceReadTests.cpp`，先证明Job-only误读I/O、单Job/单Frame调用全量GetJobs，再修复并验证。
- Job/Wait图按域限定读取；单Job和相邻依赖先选ID；timeline slice显式限定Frame Job、prerequisite与可达Gfx范围。
- 底层读取保留锁并增加取消/截止时间检查；外部前置Job完成时间表只保存所需ID。
- 最终真实MCP：Job图0.765/0.688秒，4267节点/8585边且complete=true；单Job0.125秒、依赖0.140秒；1ms预算拒绝与异步取消均约15ms，随后可继续查询。
- 原始timeline路径不再出现约19.6GiB的内存膨胀；本轮Query最高约6.46GiB。但10000节点设置下slice仍明确partial（5.438秒），不得用它冒充完整调查；完整Job证据使用graph + job.get/dependencies。
- 仅原始数组线性扫描，没有新建Job索引。其余全局Job/关联方法不在本次优化完成声明内。

Query：`build-query-devbase/Release/tracy-query.exe`；SHA-256 `9795d12e58a9c1e62b76090849533e2588374ae70833f6333f1f891a2107a5f2`。schema仍为1.35.0。旧候选缓存继续由冻结EXE读取；新定向查询用新EXE并记录身份，禁止手改cache manifest或为了更换EXE重跑完整扫描。

最终测试与边界以本地验收文档为准：

`C:/Users/Admin/Documents/JN-Unity-T3/QSCAN15-Cache-Merge-and-FW-Acceptance-20260909/FW4-Job-Directed-Query-Repair-Acceptance.zh-CN.md`。

本次解除工具层阻断，不自动把Job候选或全部Skill验收标为完成；既有恢复状态和待调查数量保持不变。没有自动提交、推送或覆盖安装版Skill。
