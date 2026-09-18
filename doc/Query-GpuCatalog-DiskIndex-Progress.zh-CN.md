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
