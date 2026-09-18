# Query 长数据静态持有者审计

更新：2026-09-18。当前索引 schema26。本文对应修订计划 P2/P3/P6，只记录源码可证实的持有者和处理状态；“候选”不等于已证明的 OOM 根因。

| 路径 | 随录制增长的对象 | 2026-09-18 状态 | 有界策略/下一步 |
|---|---|---|---|
| QueryIndex Zone 校验 | ZoneExtra 名称状态两个全量 vector；Zone/GPU 调用栈引用 vector；GPU Memory CPU-zone ordinal 两个 vector | 已移除 | schema24 用受共享预算约束的压缩bitset在写CPU表时把名称状态复制到CPU Zone空闲flag，随后立即释放bitset；验证普通Zone不再读取ZoneExtra。调用栈引用使用有界run外排、归并、去重和4096项解析批次；GPU ordinal扫描时直接写分页表；scratch完成后立即删除并释放账本 |
| QueryIndex Worker 生命周期 | 原 Worker 写出 compact 后仍与 compact Worker 重叠 | 已修正 | compact 写出和 fingerprint 完成后立即 `source.reset()`，再打开 compact；资源快照记录存活 Worker 数 |
| QueryIndex CPU 排序/section pack | 原始映射文件、排序副本、分页副本同时存活 | 部分完成 | 全部写入已计入 16S；每个 section pack 后立即删除 raw。CPU 排序仍有一个临时等尺寸副本，属于有界磁盘而非 Private Bytes |
| 全局字符串 | Worker 字符串正文、普通字符串 ID、线程名 token、外部名称 pair；消费者长期持有 `const char*` | 已从冷建及 compact 外置 | 原始字节＋NUL 的只读映射、分页 ordinal 目录、token 外排索引和三类 ID 外排索引；Query 对外返回拥有值，Worker 只使用在 Reader 生命周期内不淘汰的稳定映射地址。schema20 compact 使用 manifest 绑定标记，脱离字典会在加载时拒绝 |
| Callstack | payload、Frame/inline 定义、父调用栈和符号地址映射 | payload 与 Frame/inline 已外置；父调用栈/符号映射待迁移 | schema21 payload 使用分页 ordinal 目录和只读 frame arena；schema22 Frame 定义使用原 `CallstackFrameId` 外排索引、分页目录和只读 inline-frame arena。`ResolveCallstacks`/ghost frame 可直接读取侧车；父调用栈和符号地址映射仍需按需页化 |
| JN 固定域 | Schedule/Config/Dependency、GfxDispatch、Frame、I/O、Runtime、Script、Callsite | 待迁移 | 事实表＋键/ordinal 索引；逐域切换真实后端消费者后才从 compact 移除 |
| Tracy 域 | Frames、Memory、Messages、Plots、Locks、ContextSwitch、FrameImages、SymbolCode、SourceCache | 待审计 | 为每个容器记录输入关系和常驻上界；选择保留、分页、溢写或明确拒绝 |
| GPU Memory 摘要 | `BuildGpuMemoryAttribution`/`BuildGpuAnalysisSnapshot` 的完整 vector/map 和大 JSON | 已移出冷建；派生后端待完成 | 冷建不再打开第二个 compact Worker，也不再计算/序列化完整 Attribution；P4 独立阶段输出分页事实与小型汇总。当前按需 fallback 仍可能构建完整对象，相关入口在派生后端完成前保持未就绪 |
| Sample 派生 | ghost zone、symbol statistics、父调用栈等仍依赖 compact Worker | P4 待恢复 | 建派生索引后由 readiness 清单发布；当前继续明确 `INDEX_NOT_READY` |
| Job/Catalog 派生 | Job 聚合、依赖图、Catalog 历史、Pass 双向关联和 GPU Memory 查询 | P3质量校验部分完成；P4待恢复 | schema26沿用持久化Catalog原始质量和磁盘重验证结果；显式Resource/Allocation/Logical/String定义与Resource/Allocation/View/Logical/Part/VG/Range引用使用generation-aware外排归并。Resource/Logical名称hash和原始长度也与String定义通过32字节外排记录精确归并。Relation/Evidence校验枚举/flag，且可证明为Resource的RtasUses端点加入跨表归并；其余多态source/target仍待协议证据，再建派生索引和迁移消费者 |

P4已恢复`gpu.catalog.status`的真实磁盘后端：generation和Evidence预览有明确上界与截断字段，各域计数直接读分页目录；其余Catalog查询继续未就绪。

P6已补Windows发布临时占用重试、同轮半发布候选清理、每Trace排他owner锁、同源/schema `.tmp`崩溃残留清理和schema26 Reader代次租约。旧generation仅在取得排他租约且不属于当前manifest时回收；固定后缀白名单、指纹/schema边界和symlink/reparse检查限制删除范围。活跃Reader释放后的自动回收触发、删除失败持久化清单和长路径验收仍待完成。

## 本轮验证

- `QueryValidationStoreTests` 覆盖分页状态随机读、有界两项 run、外部归并、确定性去重、workspace 上界和磁盘回收。
- 小型 JNTRACYX v1 QueryIndex 完成 schema20 冷建、深校验和热重开，CPU Zone 218 条，压缩 sidecar 2,956 字节；118 个全局字符串保存在 5,453 字节字符串侧车，compact 文件为 2,076 字节且不再重复字符串正文。
- 冷建 manifest 明确记录 GPU Memory 完整摘要 count=0；原 Worker 写完 compact 后释放，不再为摘要打开第二个 Worker。
- schema22 callstack 真实夹具完成冷建、深校验、热开和 `callstack.resolve`，原始两帧 `0x1000 → 0x2000` 一致；该夹具 Frame 定义为空，非空/255-inline 定义由二进制侧车回归覆盖。
- Release 全目标构建通过；最终 CTest 38/38 通过（131.55秒）；compressed/sample/job/catalog 四项真实 Worker 索引集成回归通过。此前 Job 发布复现的 `MoveFileExW error 5` 仍归 P6 修复。
- schema22 正式 Trace 受控诊断运行2,700.815秒后按45分钟停止：Private Bytes峰值9,831,661,568字节，Working Set峰值38,914,113,536字节；未发生内存/磁盘错误，进程持续单核扫描约4.225亿 CPU Zone。18项未发布候选共103,992,920,640字节，已按PID清理。该轮不是性能验收。
- schema23 正式Trace范围诊断R2运行2,704.412秒后按45分钟停止；Private Bytes峰值9,844,264,960字节，无资源/数据错误，但仍未完成4.225亿CPU Zone校验。18项候选110,140,934,816字节已按PID清理；扣除验证scratch后仍比8S最终限额高4,421,743,640字节，作为P6容量风险保留。
- schema24 将名称校验状态复制到CPU Zone现有flag，缓存连续线程合法性，以64 KiB定长表识别GPU Memory标记，并仅为每类质量发现的前20条样例生成引用。Release全构建、CTest 38/38（122.40秒）、小型真实冷建/热开/`validation.run`通过；正式Trace修复后范围诊断尚未执行。
- schema24 正式Trace范围诊断R3仍在2,704.324秒达到45分钟边界；Private Bytes峰值9,771,753,472字节。停止后顺序扫描422,576,574条CPU记录确认动态名称未解析=0、非法Extra=0、线程run=28，剩余热点收敛到PlayerLoop第二次全表扫描的逐记录集合哈希。该来源判定已改成64 KiB定长表，finding键改为无分配`string_view`；Release全构建、CTest 38/38（120.13秒）和小型真实索引语义回归通过，未机械启动第四轮正式诊断。
- IndexedTraceSource原先在manifest缺少预计算Zone校验时会重新分配与全部Extra等长的名称状态和全量调用栈vector。该回退已删除；schema24把完整预计算校验作为可重开门禁，缺失或未完成时索引失效并重建。小型夹具把`precomputed`改为false后确认会重建并恢复20,000条调用栈引用告警，未把未就绪状态报告为健康。
- schema26真实小型Catalog回归确认：IndexedTraceSource持有共享Reader租约时，再发布不会删除旧generation；释放后旧generation与租约可安全回收。回收只接受20种固定拥有后缀，同前缀未知文件和其他指纹文件均保留。Release全构建与CTest 39/39通过（123.53秒）。
- Catalog名称元数据校验使用32字节定长外排记录，在`(generation,nameId)`组内先处理定义再核对Resource/Logical引用；512K记录run对应16 MiB工作区，多run归并、hash/长度破坏夹具和真实1,001资源Catalog通过。Release全构建与CTest 39/39通过（125.10秒）。
- Relation/Evidence语义回归加入健康RtasUses和DetailedEvidence记录；RtasUses已解析端点缺少同generation Resource定义会返回跨表引用异常，Relation/Evidence非法枚举或flag分别返回记录异常。定向回归与真实1,001资源Catalog通过。
- 未运行 12.43 GB 正式 Trace。
