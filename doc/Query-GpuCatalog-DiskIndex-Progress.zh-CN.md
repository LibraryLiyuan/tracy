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
