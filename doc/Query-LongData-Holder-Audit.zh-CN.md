# Query 长数据静态持有者审计

更新：2026-09-18。当前索引 schema22。本文对应修订计划 P2，只记录源码可证实的持有者和处理状态；“候选”不等于已证明的 OOM 根因。

| 路径 | 随录制增长的对象 | 2026-09-18 状态 | 有界策略/下一步 |
|---|---|---|---|
| QueryIndex Zone 校验 | ZoneExtra 名称状态两个全量 vector；Zone/GPU 调用栈引用 vector；GPU Memory CPU-zone ordinal 两个 vector | 已移除 | 名称状态使用压缩分页表；调用栈引用使用有界 run 外排、归并、去重和 4096 项解析批次；GPU ordinal 在扫描时直接写分页表；scratch 完成后立即删除并释放账本 |
| QueryIndex Worker 生命周期 | 原 Worker 写出 compact 后仍与 compact Worker 重叠 | 已修正 | compact 写出和 fingerprint 完成后立即 `source.reset()`，再打开 compact；资源快照记录存活 Worker 数 |
| QueryIndex CPU 排序/section pack | 原始映射文件、排序副本、分页副本同时存活 | 部分完成 | 全部写入已计入 16S；每个 section pack 后立即删除 raw。CPU 排序仍有一个临时等尺寸副本，属于有界磁盘而非 Private Bytes |
| 全局字符串 | Worker 字符串正文、普通字符串 ID、线程名 token、外部名称 pair；消费者长期持有 `const char*` | 已从冷建及 compact 外置 | 原始字节＋NUL 的只读映射、分页 ordinal 目录、token 外排索引和三类 ID 外排索引；Query 对外返回拥有值，Worker 只使用在 Reader 生命周期内不淘汰的稳定映射地址。schema20 compact 使用 manifest 绑定标记，脱离字典会在加载时拒绝 |
| Callstack | payload、Frame/inline 定义、父调用栈和符号地址映射 | payload 与 Frame/inline 已外置；父调用栈/符号映射待迁移 | schema21 payload 使用分页 ordinal 目录和只读 frame arena；schema22 Frame 定义使用原 `CallstackFrameId` 外排索引、分页目录和只读 inline-frame arena。`ResolveCallstacks`/ghost frame 可直接读取侧车；父调用栈和符号地址映射仍需按需页化 |
| JN 固定域 | Schedule/Config/Dependency、GfxDispatch、Frame、I/O、Runtime、Script、Callsite | 待迁移 | 事实表＋键/ordinal 索引；逐域切换真实后端消费者后才从 compact 移除 |
| Tracy 域 | Frames、Memory、Messages、Plots、Locks、ContextSwitch、FrameImages、SymbolCode、SourceCache | 待审计 | 为每个容器记录输入关系和常驻上界；选择保留、分页、溢写或明确拒绝 |
| GPU Memory 摘要 | `BuildGpuMemoryAttribution`/`BuildGpuAnalysisSnapshot` 的完整 vector/map 和大 JSON | 已移出冷建；派生后端待完成 | 冷建不再打开第二个 compact Worker，也不再计算/序列化完整 Attribution；P4 独立阶段输出分页事实与小型汇总。当前按需 fallback 仍可能构建完整对象，相关入口在派生后端完成前保持未就绪 |
| Sample 派生 | ghost zone、symbol statistics、父调用栈等仍依赖 compact Worker | P4 待恢复 | 建派生索引后由 readiness 清单发布；当前继续明确 `INDEX_NOT_READY` |
| Job/Catalog 派生 | Job 聚合、依赖图、Catalog 历史、Pass 双向关联和 GPU Memory 查询 | P3/P4 待恢复 | 先完成 Catalog 语义质量权威，再建外排索引和迁移消费者 |

## 本轮验证

- `QueryValidationStoreTests` 覆盖分页状态随机读、有界两项 run、外部归并、确定性去重、workspace 上界和磁盘回收。
- 小型 JNTRACYX v1 QueryIndex 完成 schema20 冷建、深校验和热重开，CPU Zone 218 条，压缩 sidecar 2,956 字节；118 个全局字符串保存在 5,453 字节字符串侧车，compact 文件为 2,076 字节且不再重复字符串正文。
- 冷建 manifest 明确记录 GPU Memory 完整摘要 count=0；原 Worker 写完 compact 后释放，不再为摘要打开第二个 Worker。
- schema22 callstack 真实夹具完成冷建、深校验、热开和 `callstack.resolve`，原始两帧 `0x1000 → 0x2000` 一致；该夹具 Frame 定义为空，非空/255-inline 定义由二进制侧车回归覆盖。
- Release 全目标构建通过；最终 CTest 38/38 通过（131.55秒）；compressed/sample/job/catalog 四项真实 Worker 索引集成回归通过。此前 Job 发布复现的 `MoveFileExW error 5` 仍归 P6 修复。
- 未运行 12.43 GB 正式 Trace。
