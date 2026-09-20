# Query 长数据改造：存储基础检查点

## 2026-09-18：修订计划 P0 完成，进入 P1

当前执行基线为 `2acd13ed674ba04ab87948ba959e15524d707085`、索引 schema19。r8 JNTRACYX v1 Trace 已冻结为 12,428,310,213 字节、SHA256 `DA845BBC88E15688F262F979529E5904FCFD560B4FF0936CD8B6E525C3C5935A`；旧 `1ce08c45` 只作为历史源码基线。

消费者审计确认：Zone、Sample、JobStage/Gfx/Relation/GPU Reference 七类 JN 表和原始 GPU Catalog 已有 sidecar，但 Job、Sample 派生、Catalog/GPU Memory、Evidence 和完整 Native Scan 尚未恢复。`SampleDerivedDataReady`、`JobDerivedDataReady`、`CatalogDerivedDataReady` 当前恒为 false，是防止把未迁移数据误报为空的门禁。

Query 的资源合同为 Private Bytes 16 GiB 软目标、Windows 进程提交 32 GiB 硬限制、最终派生文件 8S、构建存活峰值 16S。Working Set 只作映射页观测，不再与 Private Bytes 混作一个限额。Converter 的 20 GiB 是独立历史额度。

先前 26.83 GB 与 23.07 GB 是失败前离散采样，不是可靠峰值或根因。P1 将先接通统一 `QueryResourceContext`、写前磁盘申请、阶段化 100 ms 采样和资源错误映射；P1 门禁满足前不再启动 12.43 GB 全量冷建。

## 2026-09-18：P1 统一资源计账与失败诊断完成

- 新增单 Trace/Profile 的 `QueryResourceContext`。索引 Reader/Writer、映射表、compact 写出、压缩 scratch、摘要、manifest、深校验、Native Scan、分析缓存和响应预留共享 workspace 与磁盘账本；复用的索引/Scan 根目录可在使用前纳入同一账本。
- Private Bytes 是 16 GiB 软压力和 32 GiB 硬限制的判定指标；Working Set 仅记录映射页影响。Windows/Linux 同时记录系统 commit 上限、已用量、当前和最小余量。Worker 读取和后台加载每 100 ms 检查，阶段、子进度、存活 Worker 及主要验证容器 size/capacity 进入快照。
- CLI/MCP 支持 `--resource-profile`。Profile 的 Trace 大小/hash、8S/16S、Private Bytes、采样周期和单重型构建配置会在冷建、热开及 Scan 复用时核验；Windows 进程提交仍由启动时的 32 GiB Job Object 硬限制保护。
- Working Set 的 16 GiB OS 硬限制已移除，避免把文件映射页误当作提交内存。`bad_alloc`、workspace、磁盘和进程 Private Bytes 拒绝统一映射为 `RESOURCE_LIMIT`。
- 失败预留 64 KiB workspace 和 1 MiB 磁盘报告空间。小型真实 JNTRACYX v1 回归先以 16 KiB 峰值限额写前拒绝，清理所有候选并保留诊断；同一输入随后正常冷建、深校验和重开成功。输入 1,310,224 字节，最终派生 1,338,846 字节，目录无 `.tmp.*`。
- P1 最终 Release 全量构建通过；CTest 35/35 通过，119.63 秒。P1 完成前后均未启动 12.43 GB 全量冷建。

下一步进入 P2 静态持有者审计和小型合成压力测试。已确认的优先项仍是分页 ZoneExtra 校验状态、调用栈引用外排、原/compact Worker 生命周期、字符串/调用栈按需读取和 GPU 摘要拆分；不会把这些候选提前写成 OOM 根因。

## 2026-09-18：P2 首个有界化检查点

静态持有者清单见 `doc/Query-LongData-Holder-Audit.zh-CN.md`。Zone 校验不再分配与全部 ZoneExtra 等长的两个 Private Bytes vector，也不再累积全部 Zone/GPU 调用栈引用；名称状态改为分页表，调用栈引用采用外部 run 排序、确定性去重和 4096 项解析批次。GPU Memory 的两个 CPU-zone ordinal vector 改为扫描时直接写分页表。所有 scratch 纳入统一账本并在阶段完成后删除。

小型真实后端冷建、深校验、热开通过；新增分页校验存储测试和受影响的外部排序/预算测试 4/4 通过。P2 仍未完成：字符串、调用栈定义、其他 JN/Tracy 域和 GPU 摘要尚待迁移，未启动 12.43 GB 运行。

随后从冷建必经路径移除了完整 GPU Memory Attribution 和大 JSON：原 Worker 写出 compact 后立即释放，冷建不再为摘要打开第二个 compact Worker；manifest 将该派生摘要记录为未构建。GPU Memory 分页派生后端仍属于 P4，现阶段不会把缺失摘要误报为已完成。

## 2026-09-18：schema20 全局字符串外置

- 全局字符串正文改为磁盘字节 arena，并保留每条字符串的 NUL 边界；ordinal 目录使用分页表，原始 token、普通字符串 ID、线程名和外部双名称使用有界外部排序索引。重复 token/ID 保持原 Worker 的 first-wins 语义，缺失 token 不会被发布为有效名称。
- Query 值接口返回拥有的 `std::string`；Worker 兼容接口指向不淘汰的只读文件映射，其生命周期由字符串 Reader 和 IndexedTraceSource 明确持有。映射页计入 Working Set 说明，不计为 Private Bytes 容器。
- 冷建 Worker 不再建立 `stringData`、普通字符串、线程名和外部名称的全量内存表。compact payload 不再重复字符串正文，并写入 manifest 绑定标记；脱离 schema20 manifest/字典的普通 Worker 会立即拒绝，不能把缺失字典解释为空。
- 小型真实 JNTRACYX v1 冷建、深校验、热开及 overview 通过：输入 1,309,833 字节，118 个全局字符串侧车 5,453 字节，compact 2,076 字节。Release 全目标构建通过，最终 CTest 37/37（120.77秒）通过，compressed/sample/job/catalog 四项真实后端集成回归通过。
- P2 仍未完成：Callstack payload/Frame/父调用栈、其余 JN/Tracy 常驻域仍需按证据迁移；尚未运行 12.43 GB 正式 Trace。

## 2026-09-18：schema21 Callstack payload 外置

- Callstack payload 按原 ID 写入只读 frame arena，分页目录只保存 offset/count；单个超深 payload 支持分块写入和访问，不建立全量 `VarArray`/slab。arena 内容 hash、分页块校验和 manifest 目录 hash 分层保存。
- `WorkerTraceSource::ResolveCallstacks` 直接读取稳定映射 payload，再解析 Worker 中暂时保留的 Frame/inline 定义。compact 使用 manifest 绑定标记；无调用栈侧车时普通 Worker 在加载阶段拒绝。
- 小型真实 callstack Trace 冷建、深校验、热开和 `callstack.resolve` 通过，原始 `0x1000 → 0x2000` 两帧一致；payload 侧车755字节，compact 2,055字节。Release 全目标构建通过，CTest 38/38（160.95秒）通过，compressed/sample/job/catalog 四项真实后端回归通过。
- Job 回归首次发布时复现一次已知 Windows `MoveFileExW error 5`；检查确认压缩 Writer/Reader 均已离开作用域，新的独立代次随后通过。该偶发目录发布失败和失败后已移动代次清理仍是 P6 的明确缺陷，未通过重跑改写为“已修复”。
- P2 下一项为 Callstack Frame/inline 定义、父调用栈和符号地址映射；12.43 GB 正式 Trace 尚未运行。

## 2026-09-18：schema22 Callstack Frame/inline 定义外置

- `callstackFrameMap`、`CallstackFrameData` 和 inline frame 数组改为磁盘侧车：原 `CallstackFrameId.data` 经有界外部排序定位 ordinal，定义目录分页保存 image-name、offset/count，正文使用只读 `CallstackFrame` arena。
- `ResolveCallstacks`、父调用栈中的普通 frame 及 ghost frame 均能读取外置定义；compact Frame 表使用 manifest 绑定标记，普通 Worker 脱离侧车会拒绝。父调用栈自定义 frame 和符号地址派生仍待迁移。
- 独立回归覆盖缺失 ID、重复查找稳定地址、255 个 inline frame、跨块写入、深校验和 hash；小型真实 callstack 夹具完成 schema22 冷建、深校验、热开与两帧解析，但该夹具本身没有 Frame 定义，未把它误报为非空定义端到端证据。
- Release 全目标构建和 CTest 38/38（131.55秒）通过；compressed/sample/job/catalog 四项真实索引回归均通过。12.43 GB 正式 Trace 尚未运行。

## 2026-09-18：正式 Trace 内存诊断与 schema23 校验优化

- schema22 对12,428,310,213字节正式Trace运行一次45分钟受控诊断。Private Bytes峰值9,831,661,568字节，低于16 GiB软目标并显著越过schema19约23.07 GB失败区间；Working Set峰值38,914,113,536字节后回落，按映射页口径解释。
- 诊断没有资源或数据错误，但未在45分钟内完成：CPU时间2,601.375秒，持续单核扫描约4.225亿CPU Zone。停止时18项未发布候选共103,992,920,640字节，已按本轮PID清理，manifest未发布。该轮是非性能诊断，不计入最终综合验收。
- 代码审计定位到 CPU Zone 校验对每条记录调用分页 `ExtraNameState` 随机读。schema23 将名称active/resolved状态固化到ZoneExtra flag，移除该分页随机读，同时保留index0未解析和无动态名称回退语义。
- schema23 Release全目标构建、小型真实压缩索引回归及CTest 38/38（130.67秒）通过。下一步只执行一次修复后范围重跑，验证校验耗时；父调用栈/符号映射和P6事务发布仍未完成。

## 2026-09-18：schema23 R2 证据与 schema24 校验热路径

- schema23修复后范围诊断R2仍在2,704.412秒达到45分钟停止条件。Private Bytes峰值9,844,264,960字节，Working Set峰值47,719,985,152字节；没有资源拒绝、数据错误或manifest发布，进程持续单核工作。分页名称状态不是唯一吞吐瓶颈。
- 停止时18项、107个文件的未发布候选共110,140,934,816字节，已在确认PID退出、目录所有权和无reparse point后清理。扣除6,292,709,472字节验证scratch，其余候选仍比8S最终限额高4,421,743,640字节；最终发布前必须由阶段回收或布局优化消除。
- schema24在ZoneExtra流式写入阶段用三组压缩bitset保存text/name状态，受共享workspace预算约束；写CPU表时将name active/resolved和非法Extra引用复制到现有CPU Zone flag，CPU表完成后立即释放bitset。验证普通Zone不再读取8.37 GB ZoneExtra映射。
- CPU校验缓存连续线程合法性，GPU Memory标记使用64 KiB定长表；发现计数仍完整累计，但每类只在需要保留前20个样例时构造实体ref，避免数亿条相同告警反复生成字符串。
- 先失败合同回归已转绿；Release全目标构建、CTest 38/38（122.40秒）通过。1,310,224字节真实夹具schema24冷建819毫秒，热开overview和`validation.run`成功；其中20,000条未解析调用栈仍准确计数且只返回20个样例ref。
- schema24尚未运行正式Trace修复后范围诊断。父调用栈/符号映射、P3/P4消费者、P5分页恢复、P6事务发布和8S容量处理仍未完成。

schema24正式Trace范围诊断R3仍在2,704.324秒达到45分钟停止条件。Private Bytes峰值9,771,753,472字节，未发生资源或数据错误。停止后的CPU表字段统计覆盖422,576,574条记录：动态名称51,928,518条且未解析为0，非法Extra为0，时间不完整5条，排序后线程run仅28个。这排除了名称finding和线程哈希作为本轮主因。

剩余热点是`ValidateMainPlayerLoopRoots`对全表第二次扫描时，每条Zone对两个`unordered_set<int16_t>`执行查找。修复将Main.PlayerLoop/PlayerLoop来源编码为64 KiB定长状态表，父链检查也直接索引；finding代码表改用`string_view`避免异常Trace的临时字符串分配。存储布局不变，schema保持24。Release全构建、CTest 38/38（120.13秒）及小型真实冷建/validation语义回归通过。R3证据、字段分布和清理记录位于`Query-Acceptance-20260918/diagnostics/P2-ZoneValidation-CpuFlags-R3`；没有机械启动第四轮正式诊断。

同时移除了IndexedTraceSource中仅在manifest未预计算校验时触发的全量`extraNameState`和调用栈vector回退。schema24现要求已发布manifest包含完整预计算校验；缺失或未完成会使索引失效并走受控重建。小型夹具将`precomputed`改为false后确认新代次重建，`validation.run`恢复原16项finding和20,000条未解析调用栈计数，未把缺失质量权威静默报告为健康。

schema26进一步把Main.PlayerLoop语义检查并入原有CPU Zone校验遍历：普通Zone只做64 KiB来源表索引，只有实际Main.PlayerLoop根才读取父链；根区间按线程收集、排序并检查交叉，不再调用第二次4.225亿记录全表扫描。根集合以共享workspace保守计账，每1,024条扩容检查，CPU阶段后立即释放并清零容器指标。小型真实Catalog索引的`zone_validation`与改造前schema26基线JSON完全一致；Release全构建与CTest 39/39通过（127.31秒）。该证据足以删除已定位的第二遍，不机械启动R4；正式Trace是否能完成仍留给计划中的综合验收。

## 2026-09-18：schema25 Catalog 磁盘语义质量权威

- 新增有界Catalog语义校验器：分页读取12个固定域和字符串域；校验batch kind/encoding、全域范围与覆盖、transport checksum存在性、stored checksum、payload字节、generation引用及汇总、Control/Batch合并后的连续序列、字符串hash和字符串generation归属。
- generation/sequence使用受共享workspace和磁盘预算约束的外部排序；checksum工作区固定1 MiB。scratch完成后删除并释放账本；Windows测试首次因排序输入句柄仍打开而拒绝清理，修正为显式关闭后再删除，没有把文件占用误报为清理成功。
- manifest同时保留原始present/valid和完成后的重验证healthy/findings；深校验重新计算语义结果并与manifest逐项比较。缺失、原始无效、重验证异常和健康状态已可区分。Catalog能力在语义失败时不会说明为健康可用。
- 健康夹具通过；修改batch范围、stored checksum、batch generation或字符串generation的夹具分别返回`CATALOG_BATCH_RANGE`、`CATALOG_STORED_CHECKSUM`、`CATALOG_GENERATION_REFERENCE`和`CATALOG_STRING_GENERATION`。
- 小型真实Catalog Trace的1,001条资源完成schema25冷建、发布、重开和深校验，manifest质量为complete/present/original_valid/healthy且findings为空。Release全构建及CTest 38/38（125.92秒）通过。
- 显式跨表引用已使用独立外排文件按`(generation,id)`精确归并：Resource→Allocation/String、View/Logical/Part/VG/Range→Resource、Part→Logical；破坏夹具返回`CATALOG_CROSS_TABLE_REFERENCE`。外排临时文件纳入同一磁盘/工作区预算并在句柄关闭后回收。小型真实1,001资源Catalog再次通过发布和深校验；Release全构建与CTest 38/38（124.78秒）通过。
- Resource/Logical名称引用新增32字节定长外排记录，按`(generation,nameId,definition/reference,hash,length)`排序归并；引用端`nameHash/nameOriginalLength`必须与Catalog String定义一致，定义冲突同样报告`CATALOG_NAME_METADATA`。工作区每个排序run最多16 MiB，所有run和源文件纳入共享磁盘账本并随语义scratch回收；hash与长度双破坏夹具、多run归并、1,001资源真实Catalog及Release全构建/CTest 39/39（125.10秒）通过。
- Relation记录现校验operation/relation/exactness/flags枚举范围；Query现有`gpu.resource.raytracing_chain`明确把`RtasUses`两端解释为Resource ID，因此已解析端点按generation加入Resource外排引用归并，未解析pointer flag端点保留原有incomplete语义。DetailedEvidence校验kind/state和已定义flag掩码，非法记录返回`CATALOG_EVIDENCE_RECORD`。RTAS缺失资源与Evidence非法枚举破坏夹具转绿，1,001资源真实Catalog再次通过。
- P3尚未完成：其他Relation和DetailedEvidence的source/target仍是按relation/kind解释的多态ID；v1记录在Worker解析后会清除部分指针类型flag，当前源码不足以对所有已解析端点恢复原类型，因此只继续实施有协议证据的逐类校验，不以猜测制造误拒绝。P4其余查询派生索引和消费者迁移尚未开始。

## 2026-09-18：P4 首个消费者——磁盘 Catalog 状态

- `gpu.catalog.status`不再被全域`CatalogDerivedDataReady=false`一起阻断。IndexedTraceSource新增只读状态接口，直接从Catalog分页侧车读取各域精确计数、generation汇总和最多16个Evidence checkpoint预览，并复用schema25持久化语义质量。
- generation响应最多4,096条，Evidence内部扫描最多65,536条；截断分别通过`generations_truncated`和`detailed_evidence_preview_truncated`明确返回。输出vector预留纳入共享workspace，generation总计以顺序扫描计算并检查uint64溢出/取消。
- 其他`gpu.resource.*`、`gpu.memory.*`、Pass和`gpu.catalog.validation`仍返回`INDEX_NOT_READY`，没有因状态查询恢复而误开放完整快照路径。
- 小型真实Catalog索引返回present/valid/complete、1个generation、1,001条resource和零质量finding；直接请求`gpu.resource.search`仍明确未就绪。真实后端状态合同通过，Release全构建与CTest 38/38（120.71秒）通过。

## 2026-09-18：schema27 Resource点查与生命周期

- 冷建在Catalog语义校验通过后生成`resource-summary`：原始Resource记录以`(resourceId,ordinal)`外排，generation使用顺序分页表，不构建全量resource map；每个Resource只保存最新定义、定义generation、create/destroy边界和open-boundary状态。`resourceId`有序分页表支持O(log N)点查。
- Catalog String生成`(generation,nameId,ordinal)`有序分页索引，`gpu.resource.get/lifetime`按点查读取名称，不扫描或加载全字符串表。构建run使用共享workspace/disk预算，scratch完成即回收；深校验检查分页块、Resource严格递增和String ordinal身份。
- 内部索引升至schema27，旧schema26保持隔离并受原有Reader lease保护。`gpu.resource.get`和`gpu.resource.lifetime`走统一磁盘后端；`gpu.resource.search`基础名称/primary-kind/resource-class过滤直接顺序读取ID有序摘要，v2 cursor绑定逻辑offset和下一物理ordinal，后续页不重扫已消费前缀。需要Part/Range/Logical/alias/共享Allocation反向索引的五种布尔过滤继续明确`INDEX_NOT_READY`。
- 组件夹具覆盖命中、缺失、名称和生命周期字段；1,001资源真实Catalog完成构建、发布、深校验、重开和Reader生命周期。真实`gpu.resource.get`对resource 1返回4,194,304字节capacity与generation `14154813628825468929`，其完整data JSON与同Trace非索引基线逐字段相同。Release全构建与CTest 39/39通过（127.21秒）。
- 同一真实夹具的1,001条Resource记录归并为1个resourceId生命周期；Indexed基础search首屏的资源、total和字段与非索引基线data完全一致。分页Reader在IndexedTraceSource生命周期内复用，不为每条记录重开分页文件。基础search接入后Release全构建与CTest 39/39通过（124.74秒）。

## 2026-09-20：schema28 Resource关联过滤

- `gpu.resource.search`的`has_mesh_parts`、`has_ranges`、`has_logical_bindings`、`has_alias_group`和`has_shared_allocation`已迁移到48字节定长的按`resourceId`有序关联摘要；查询按Resource物理ordinal点读，不重建旧Worker中的全量map/set。
- Part、Range和Logical计数写入统一关联事实外排。Alias语义按`logicalResourceId`分别外排alias声明和Resource绑定后归并，保留“alias声明与绑定可位于不同历史记录”的旧语义。共享Allocation仅使用每个Resource的最新定义，以`(allocationId,resourceId)`外排分组获得精确资源数。
- 所有source/sorted run、generation临时表和分页scratch使用共享workspace/磁盘预算。检查真实产物时发现schema27以来`resource-index-scratch`会因generation Reader仍持有句柄而残留；schema28缩短句柄生命周期，并把删除失败从忽略改为构建失败和候选清理。新真实产物不含该目录。
- 新增真实`SessionManager → QueryService`合同测试，逐项验证五种过滤和返回计数字段。1,001条Resource真实Catalog完成冷建、深校验、重开及与旧Worker关联语义逐资源对照；该夹具聚合为1个resourceId，关联分页表337字节，Catalog section 28,402字节，完整独立输出81,187字节。
- Release全构建和CTest 40/40通过（124.08秒）。本检查点没有运行12.43 GB正式Trace，也不计入约定的单轮综合性能验收。Resource explain和关联详情页、Pass、GPU Memory、Job、Sample派生及Scan迁移仍待继续。

## 2026-09-18：P6 Windows 发布重试与半发布清理

- QueryIndex的文件/目录原子移动对Windows `ERROR_ACCESS_DENIED`、sharing violation和lock violation进行最多2秒的有界退避重试；每次重试检查取消。其他错误立即失败，不掩盖错误码。
- 新回归真实持有源目录句柄：80 ms临时占用后发布成功；永久占用在3秒内失败且目标不存在；取消在500 ms内停止并返回cancelled。
- 构建发布时记录每个已从`.tmp.<pid>`移动到最终generation名的候选。如果manifest发布前后续步骤失败，按逆序及Trace父目录、名称前缀、symlink/reparse检查删除已移动候选，避免遗留“无manifest但看似最终”的孤儿代次；删除成功后磁盘账本同步释放。
- 小型真实Catalog索引发布/重开仍通过；Release全构建及包含新增发布测试的CTest 39/39（125.57秒）通过。
- 构建全生命周期新增每Trace排他owner锁。Windows使用无共享句柄，文件存在但无持有者的崩溃残留可重新取得；并发持有时第二个构建明确拒绝。成功或受控失败释放句柄并清理锁文件，小型真实发布后无owner文件残留。
- owner锁接入后的Release全构建及CTest 39/39通过（126.17秒）。
- 取得owner锁后会扫描Trace目录的直接子项，只匹配当前完整源指纹前16位、当前schema及`.tmp.<pid>`命名的崩溃候选；reparse/symlink和其他指纹数据不会删除。候选先加入共享磁盘账本，删除成功后释放；扫描、计账或删除失败会中止新构建并保留错误。
- 身份边界回归确认同指纹文件/目录被清理、其他指纹候选保留；小型真实Catalog构建后无`.tmp`或owner锁残留。
- schema26为每个已发布generation增加跨进程Reader租约。`Validate`在读取任何generation文件前取得共享租约，`Open`把租约保持到IndexedTraceSource析构；重建发布新manifest后，只有能取得排他租约的旧generation才会回收，活跃Reader继续读取原不可变文件。
- 回收只扫描当前完整源指纹前16位和schema26的直接子项，并将20种已知section/lease后缀作为固定白名单；当前manifest所指generation、其他指纹、未知同前缀文件、symlink及reparse point均不会删除。schema25及更早索引不会被schema26回收，避免旧进程未持租约时发生交叉删除。
- 1,001资源真实Catalog完成`Build → Open → 再次Build`：第二次发布时旧generation因活跃Reader被完整保留；释放Reader后定向回收删除旧generation和租约，当前generation保持可用。Release全构建及CTest 39/39通过（123.53秒），删除白名单回归随后单独通过。
- 新构建取得owner锁后、加载Worker前，会读取同源/schema26当前manifest的generation并排除它，再回收其他无活跃Reader的最终代次；manifest缺失时回收无引用代次，manifest损坏或身份不匹配时保守跳过起始回收。真实Catalog回归在旧Reader仍活跃时注入孤儿最终文件，首次Worker回调前确认孤儿已删除且当前代次仍存在。
- 延期或失败清理现在原子发布`<trace>.jnidx.cleanup.json`：最多列出256个仍存直接子项、精确`retained_count`和截断标记，reason限制4 KiB、整份报告限制512 KiB；写入使用共享磁盘账本，清理完成后删除报告并释放额度。
- Windows占用验收直接以不共享delete的句柄锁住孤儿generation文件：下一次Build在Worker加载前受控失败，当前manifest/Reader保持可用，报告为`state=failed`且包含仍存项；释放句柄后重建成功，活跃旧Reader报告转为`deferred`，最终释放并回收后报告消失。Release全构建与CTest 39/39通过（125.11秒）。
- QueryIndex的Build/Validate/CreateResourceContext/ManifestPath入口统一转为Win32 extended-length绝对路径；owner/Reader/cleanup lease、原子MoveFileEx、属性检查、manifest读取及映射文件句柄均使用同一`AnalysisIoPath`。普通短路径调用方无需预先加`\\?\`。
- 334字符真实Catalog目录完成创建、复制、三次Build、深层sidecar发布、Reader租约、Windows占用失败报告、延期、回收和重开；此前同一路径在`create_directories`即失败。Release全构建与CTest 39/39通过（126.75秒）。
- Reader lease析构会先关闭共享句柄，再非阻塞取得同Trace owner锁并重新读取manifest；若自身generation已退休，则尝试排他租约回收并更新cleanup报告。builder仍活跃、其他Reader仍持有或manifest身份异常时不抢占，保留既有延期证据和下一次Build恢复路径。
- 双Reader真实Catalog回归确认：第一个Reader释放后旧generation和`deferred`报告仍存在；最后一个Reader释放后旧generation、lease和cleanup报告立即消失，当前generation继续可用。Release全构建与CTest 39/39通过（124.78秒）。P6 generation生命周期组件已闭环；正式12.43 GB运行的8S最终容量和端到端发布仍是整项计划的未完成门禁。

2026-09-15，基于1ce08c45，工作分支feature/JNTracy-GpuCatalog-DiskIndex。本检查点不是GPUCatalog长数据改造完成声明。

## 已实现并验证

- 新增有界压缩定长记录存储：Zstd/raw块、磁盘ordinal目录、随机/跨块读取、身份与内容校验；不整体加载目录。支持按调用方预算计账和取消。
- Query索引schema14，将CPU/GPU Zone、extra、Gfx/Relation和GPU Reference等固定记录sidecar压缩；代次独立命名并最后发布manifest。按值读取，避免块切换后引用失效。
- Native Scan逐帧序列使用压缩记录存储，保留逐签名checksum、帧号和原始数值；bundle发布、重开与策略重算接入新存储。
- Windows Query入口增加32 GiB进程提交内存限制；索引工作集辅助限制改为16 GiB。内存压力回收策略与启动Profile完整传递仍待完善。
- 添加8S/16S磁盘限额原语，Scan启动及缓存构建峰值开始使用Trace相对限制。加载索引与所有辅助产物的统一计账、最终8S发布门禁尚未完成。
- 修复GPU内存请求关联：过期的内层区间不再遮住仍有效的外层区间；用区间索引避免逐分配倒扫全部未来请求。此为独立正确性修正，不归入压缩收益。

## 校验

- 全部功能CTest：30/30通过，176.44秒；各新能力保留红/绿回归。
- 非Catalog既有测试Trace：349639120字节。新加载索引516204374字节，约1.4764倍；旧加载索引2120015128字节。CPU Zone仍为9854660条，CPU sidecar为105851142字节。已通过深校验和重开。
- 上述只衡量加载索引，不是整个Query的最终占用，也不是新30分钟Catalog录制的结果。未开始基线/新版单轮性能初评。
- 功能构建使用VS18 x64 Release /O2 /EHsc /bigobj、关闭LTO以降低迭代成本；正式性能比较仍需统一构建配置，不能混用结果。

## 输入阻断

独立30分钟stream的原生日志校验通过：746717条日志记录，Protocol90，完整。旧build-latest转换器为2026-08-25构建，慢速legacy尝试已在候选写出前终止并保留记录。

切换包含离线Catalog修复的工具后显著提速，但最后成功采样为61872/226710个client records（约27.3%）、提交内存约25 GiB，随后以exit5报告系统内存压力超限。未得到可用.tracy。转换器磁盘支撑属于新增范围，已向用户请求确认，尚未实施。

## 未完成

统一全部Query产物的8S/16S计账与发布门禁、完整启动资源Profile、Catalog原始表外置与质量权威迁移、所有定向查询与Scan迁移、GPU内存大JSON摘要拆表、真实输入转换及单轮初评。

详细执行计划及证据位于工作区reviews/query-longtrace-gpucatalog-20260914和GpuCatalog-DiskIndex；原始录制及历史分析保持不变。未合并DevBase、未推送、未替换正式工具。

## 2026-09-17：转换完成后的 Query 续作

分支已包含 Converter 的磁盘外置、JNTRACYX v1 写出与30分钟真实录制验收。正式输入为12428310213字节；转换阻断解除。冻结 Query 对该文件仍以 `RESOURCE_LIMIT: not enough memory to load trace` 失败，因此 Query 冷启动仍是独立阻断。

第一个续作检查点新增 Query Sample sidecar，索引 schema 升至16：

- 主 Sample 与持久化上下文 Sample 按块压缩，线程/种类目录与记录正文均可冷重开、身份校验和有界范围分页；上下文 Sample 的原始乱序保持不变。
- manifest 记录 sidecar 的文件身份、物理记录数，以及 Query 1.35 对外的主 Sample/派生上下文 Sample 计数。深校验覆盖两层分页表。
- 小型真实 Sample Trace 的完整 QueryIndex 构建、manifest 发布、重开及6条 `sample.list` 事实逐字段等价通过。
- Converter 离线验证 mask 仍不保留 Sample；相关 Worker 语义回归通过。Query CompactIndex 当前暂时同时保留 Worker Sample，以维持 ghost zone、符号统计等尚未迁移的能力。

这个检查点建立了 Sample 外置格式，但尚未降低 Query 的 Sample 峰值。下一步必须迁移派生上下文 Sample、ghost zone、符号统计等消费者后，才能关闭 Compact Worker 中的完整 Sample 驻留并用12.43 GB真实输入验证冷启动。

随后完成 schema17 Sample 逻辑视图：按线程ID升序，将主 Sample 与“时间等于同线程切入区间起点”的派生上下文 Sample 合并，单独保存物理引用和逻辑跨度。小型真实 Trace 对6条 Sample 的 ref、线程、时间、调用栈、种类及分页顺序逐字段等价；原始持久化上下文 Sample仍保存在物理表中，不与Query派生口径混淆。Compact Worker仍暂时保留Sample，ghost zone与符号统计迁移尚未完成。

## 2026-09-17：解除 Sample 与 JobStage 驻留后的真实诊断

schema18 已从 Compact Worker 中移除完整 Sample 表；`sample.list`、主/派生上下文计数与稳定ordinal改由Sample逻辑视图提供。尚未迁移的ghost zone和sample符号统计不会静默返回空结果，而是以 `INDEX_NOT_READY` 报告。小型真实Sample Trace确认compact payload的Sample计数为0、indexed结果仍逐字段等价。

JobStage也改为允许超大记录数的sidecar，compact payload不再保留全表；当前Job聚合消费者尚未迁移，因此Job、Job Gfx及依赖Job的Evidence能力明确标记未就绪并返回 `INDEX_NOT_READY`。小型JNTRACYX夹具验证3条JobStage的sidecar、compact移除和indexed计数一致。

使用正式12428310213字节Trace执行一次非性能“索引构建诊断”：旧冻结Query在加载期失败；schema18版本已越过Sample与JobStage，最近一次观测Private Bytes约26832334848字节、Working Set约21071273984字节，随后仍以 `not enough memory to load trace` 失败。失败后本轮所有 `.s17/.s18` 临时索引均清理。此结果不是计划中的新版性能初评；下一确定方向是Catalog、字符串/调用栈及其派生结构继续外置。

## 2026-09-18：Catalog原始表外置

schema19新增GPU Catalog存储：12个固定域、Catalog字符串域和质量元数据分别使用压缩分页表，缺失域也发布空表；支持随机点读、冷重开、Trace身份拒绝与全块深校验。小型真实JNTRACYX夹具验证1001条资源及Batch/String计数，compact payload不再保留Catalog。Catalog分析索引与MCP消费者尚未迁移，当前明确返回INDEX_NOT_READY。

正式Trace非性能索引诊断最近观测Private Bytes约23068401664字节、Working Set约51385528320字节，随后以bad allocation停止；相较schema18最近观测Private Bytes约减少3.76 GB，临时索引清理完成。下一内存层为全局字符串、调用栈和剩余派生映射；仍未进入正式单轮性能初评。
