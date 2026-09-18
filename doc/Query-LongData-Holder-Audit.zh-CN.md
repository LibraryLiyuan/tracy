# Query 长数据静态持有者审计

更新：2026-09-18。基线：`8b367d85`，索引 schema19。本文对应修订计划 P2，只记录源码可证实的持有者和处理状态；“候选”不等于已证明的 OOM 根因。

| 路径 | 随录制增长的对象 | 2026-09-18 状态 | 有界策略/下一步 |
|---|---|---|---|
| QueryIndex Zone 校验 | ZoneExtra 名称状态两个全量 vector；Zone/GPU 调用栈引用 vector；GPU Memory CPU-zone ordinal 两个 vector | 已移除 | 名称状态使用压缩分页表；调用栈引用使用有界 run 外排、归并、去重和 4096 项解析批次；GPU ordinal 在扫描时直接写分页表；scratch 完成后立即删除并释放账本 |
| QueryIndex Worker 生命周期 | 原 Worker 写出 compact 后仍与 compact Worker 重叠 | 已修正 | compact 写出和 fingerprint 完成后立即 `source.reset()`，再打开 compact；资源快照记录存活 Worker 数 |
| QueryIndex CPU 排序/section pack | 原始映射文件、排序副本、分页副本同时存活 | 部分完成 | 全部写入已计入 16S；每个 section pack 后立即删除 raw。CPU 排序仍有一个临时等尺寸副本，属于有界磁盘而非 Private Bytes |
| 全局字符串 | Worker 字符串正文、普通字符串 ID、线程名 token、外部名称 pair；消费者长期持有 `const char*` | 待迁移 | 字节表＋ordinal 目录＋三类 token 映射；对外返回拥有值或页句柄；先列出全部裸指针消费者 |
| Callstack | payload 已有 Converter 外置路径；Frame/inline 定义、父调用栈和符号地址映射仍由 Worker 持有 | 待迁移 | 按原 ID 建事实表和按需页；保留缺失引用语义；迁移 Sample/Zone/Job/Evidence 消费者 |
| JN 固定域 | Schedule/Config/Dependency、GfxDispatch、Frame、I/O、Runtime、Script、Callsite | 待迁移 | 事实表＋键/ordinal 索引；逐域切换真实后端消费者后才从 compact 移除 |
| Tracy 域 | Frames、Memory、Messages、Plots、Locks、ContextSwitch、FrameImages、SymbolCode、SourceCache | 待审计 | 为每个容器记录输入关系和常驻上界；选择保留、分页、溢写或明确拒绝 |
| GPU Memory 摘要 | `BuildGpuMemoryAttribution`/`BuildGpuAnalysisSnapshot` 的完整 vector/map 和大 JSON | 待拆分 | 从冷建必经路径移出；独立阶段输出分页事实与小型汇总，生命周期/Range 并集允许溢写 |
| Sample 派生 | ghost zone、symbol statistics、父调用栈等仍依赖 compact Worker | P4 待恢复 | 建派生索引后由 readiness 清单发布；当前继续明确 `INDEX_NOT_READY` |
| Job/Catalog 派生 | Job 聚合、依赖图、Catalog 历史、Pass 双向关联和 GPU Memory 查询 | P3/P4 待恢复 | 先完成 Catalog 语义质量权威，再建外排索引和迁移消费者 |

## 本轮验证

- `QueryValidationStoreTests` 覆盖分页状态随机读、有界两项 run、外部归并、确定性去重、workspace 上界和磁盘回收。
- 小型 JNTRACYX v1 QueryIndex 完成冷建、深校验和重开，CPU Zone 218 条，压缩 sidecar 2,956 字节。
- 受影响回归 4/4 通过：validation store、external exact statistics、workspace budget、resource context。
- 未运行 12.43 GB 正式 Trace。
