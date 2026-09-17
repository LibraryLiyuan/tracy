# Query 长数据改造：存储基础检查点

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
