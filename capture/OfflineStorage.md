# 离线转换定向存储

本实现只外置展开后的 GPU Reference Use、规范化后的 Job Stage 和转换期 ResourceSet 定义。其他 Worker 数据及最终 Catalog 解析继续使用原有内存模型。普通 `.tracy` 字段顺序、协议和 Query Job 的默认内存读取策略保持不变。

## 存储和校验

- `TracyStorageCore` 不依赖 Worker 或 Query；包含压缩记录表、资源控制器和 ResourceSet 索引。
- 两张事实表采用 4 MiB 解码块、Zstd level 1 或 raw。空域不创建文件。封存后用权威逻辑数量写出，分块计算和核对字段 SHA-256。
- ResourceSet 正文采用 128 KiB 块，支持未封存尾部和跨块定义。ID 索引采用 16 KiB B+树页，索引缓存最多 32 MiB，正文缓存最多 64 MiB。超过进程软目标后降为 1 MiB/4 MiB，不删除定义。
- 封存验证全部索引页及正文；重复 ID、非法成员、损坏、偏移越界均拒绝。索引不支持进程中断后的续算。
- Writer 的文件头、块长和压缩数据均在实际写入前计账，检查短写、刷新和关闭。解压线程错误回传主线程，避免损坏输入导致等待不结束。
- 显式转换校验 sink 才能接受超过一亿条的这两张表，并要求数量与重放预期一致。Query Job sink 明确保持关闭，普通加载继续执行原数量政策。

## 资源和发布

`3 × 单个源 stream 字节数` 是新增文件的同时刻上限；`--disk-budget` 只能收紧。所有本次临时表、索引、候选、最终输出及元数据进入同一簇大小计账。卷剩余空间在数据扩展后仍保留 2 GiB；少量预留的失败元数据可使用该安全余量，但仍不能突破总额度。

新存储共享 256 MiB 工作区，包含固定 2 MiB 控制和留证开销。进程软目标 16 GiB、Windows Job 提交硬限制 32 GiB。监控每 100 ms 使用 Private Bytes 和真实系统 CommitTotal/CommitLimit，保留系统 commit 2 GiB 余量。Tracy Vector 和 Slab 的分配失败不再提交错误容量状态，Vector 的空指针分配会抛出可捕获异常。

输入在运行期间持有只读共享句柄，拒绝并发写入；同一输入的转换通过命名互斥量串行。输出与 stream 必须同目录；正式输出已存在时拒绝覆盖。

默认元数据为：

- `<output>.progress.json`：最新阶段和轻量进度。
- `<output>.conversion.json`：成功时的计数和各阶段耗时。
- `<output>.resources.json`：成功/失败、资源峰值、各存储文件保守计账、未迁移 JN 容器容量摘要及清理残留。
- `<output>.complete.json`：输出有效且清理完成后的最后发布标记。

临时表位于 `<output>.work`，候选为 `<output>.converting`。校验通过后才发布输出和原合同 snapshot-map；映射使用原子替换，失败时保留旧映射。所有句柄关闭后清理本次非保留文件，删除成功才释放额度。资源报告中的磁盘值采用保守簇预留，包含报告及完成标记的预留，并非压缩载荷字节总和。

扫描缓存只读复用已有有效证据，不创建新的大扫描缓存，也不删除历史缓存。本受管路径拒绝 `--keep-failed-output`、`--purge-cache` 和重型 `--memory-attribution`，避免违背本阶段的清理和轻量初评约束。

强制杀进程、断电等不能执行析构的情况不承诺即时清理。再次启动会拒绝已有工作目录/候选，明确要求检查残留，不把未完成索引当可恢复状态，也不静默把残留排除后继续增长。

## 验证入口

capture 的 CTest 使用 `-R '^tracy-'`，排除第三方依赖自行登记、但未构建的压缩库 benchmark/fuzzer。新增回归覆盖资源拒绝/释放、文件占用、空域、稀疏 ID、B+树根扩展、跨块/尾块、封存损坏、短写、压缩线程异常、长路径和大数量 Reader。

`tracy-offline-worker-semantic-tests <1001-resource fixture.tracy> <independent-output-directory>` 对实际 Worker 处理函数执行旧内存/新磁盘逐字段对照，覆盖 65,535 成员、直接与集合 Use 交错、重复使用、PassEnd、重复定义、Job 调用栈 Stage，并验证普通与分块重开。

真实长录制只做一轮初评；结果不能替代 Query 长数据验收，也不能证明剩余 Worker 数据已变为有界存储。
# ZoneExtra mutable offline pages

Offline conversion additionally stores ZoneExtra records in fixed 53,248-byte
pages (4 KiB header plus 4,096 twelve-byte records). Logical IDs locate slots
directly; no per-record in-memory index is needed. The cache holds at most 600
pages, with reservations for page and cache metadata in the existing shared
256 MiB workspace. Under soft memory pressure it shrinks to 64 pages.

Updates use a local value and explicit writeback; a new Zone index is published
only after append succeeds. This covers delayed callsite resolution as well as
text/name/color/value and callstack events. The original Full/live reference
accessors remain in-memory; offline internal reads use value access.

Dirty pages are checked on write, checksummed on reload, flushed and closed at
seal. Only sealed records are visited for hashing and ordinary-format output.
The conversion validation sink explicitly streams the ZoneExtra domain even
when CPU zones are loaded, checks the final byte hash/count/order and verifies
CPU Zone extra indices. Normal readers and Query retain their default policy.
The fixed 32-bit Zone index limit is checked instead of silently truncating.

The page file is conversion-local, charged to the existing 3J budget and removed
by the same cleanup owner. It is not a recovery format. Other Worker domains
remain in memory, so this change does not promise full conversion within 20 GiB.
# Relation append-only offline storage

Offline conversion routes normalized 37-byte JnRelationData records to the
existing compressed paged record store. The original vector remains empty;
record ordering, duplicate events, thread attribution and all field bits are
preserved. Full/live and ordinary Query loading retain their existing policy.

The table is created lazily, sealed before ordinary-format output, and read in
bounded batches. Conversion validation explicitly consumes Relation alongside
JobStage and ReferenceUse, checking count, order and final SHA256. Missing
nonempty domains and incomplete empty domains fail validation; older absent
domains are allowed only when their expected count is zero. A large count is
allowed only for an explicitly requested sink and a matching expected count.

Buffers and codec scratch use the existing shared workspace. The paged store
has an optional pre-allocation callback for fresh conversion OS memory checks;
normal callers leave it unset. Files use the existing conversion ownership,
disk budget and cleanup lifecycle. Reports include the relation record count,
backend state, sealed hash, and original vector size/capacity.

This does not externalize gpuCatalogRelations or other remaining Worker tables.
