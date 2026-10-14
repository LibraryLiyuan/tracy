# 验证体系 真实案例与性能记录

验收必须固定输入、源码、二进制、配置和判定范围。以下是已有证据的整理，本轮没有重跑产品测试。

## 验证层次

| 层次 | 主要检查 | 不能替代什么 |
|---|---|---|
| 编译/静态 | 构建、接口、格式、链接、脚本语法 | 实际采集和场景运行 |
| 小型行为/契约 | 生命周期、顺序、身份、分页、错误、取消、负向数据 | 真实长录制与未触发域 |
| 真机功能 | Editor/Player、重连、图像、资源、MCP/GUI 对应 | 严格开销或优化收益 |
| 差分/一致性 | 同源记录字段、单位、ID、关系、边界与 checksum | 没有采到的生产者数据 |
| 容量/性能 | 大输入、内存/磁盘、恢复、查询与录制开销 | AI 的因果判断 |
| 报告/展示 | 候选覆盖、源码、正文保留、定位、图片、交互和可读性 | 运行时根因与实际收益 |

## 10 月 13 日同一录制的两条路线

| 项目 | 早期 Query 替代路线 | 后续完整 analyze 路线 |
|---|---|---|
| 共同输入事实 | 31,373 个完整帧、422 个严重帧、最大约 308.980 ms | 同左 |
| 候选 | 124,918 个跨域候选 | 28,995 个帧候选与 444 个长事件候选 |
| 窗口 | 425 个 CPU 窗口 | 789 个 CPU/GPU 窗口 |
| 阅读观察 | 24 条 | 28 条、6 册、33 个原生视图 |
| 范围 | 原 analyzer 内存受阻后的替代流程 | 修订 analyzer 的完整 Stream 流程 |

候选数量口径不同，不能直接作优劣比例。对比报告核对的 21 条共同 CPU 路径帧内耗时一致，15 条可比 self 一致；P99 小差异来自算法与显示精度，完整事件与帧内交集也需要分开。[两次性能分析结果对比.md:1](C:/Users/Admin/Documents/JN-Unity-T3/Share/T3两次性能分析结果对比_20261013/两次性能分析结果对比.md:1)

新版增加了 Awake 同步加载资源、Character Shader 身份、VT Feedback 和其他证据，但遗漏了部分旧版有效细节。纹理 65,536 字节/HeapType=1、描述符新旧块大小等已在回执中出现，正文仍未充分使用。扫描更完整不能自动证明报告包含全部有效发现。

主报告仍保留源码版本、Job 依赖等缺口；分析目录 README 记录交互/显示验收为部分完成。1404 项独立校验、351 个链接检查不应被解释为全部根因或全部展示场景通过。[README.md:1](C:/Users/Admin/Documents/JN-Unity-T3/Analysis/T3-115721-Full-SKILL-20261013/README.md:1)

## 执行时间和内存

后续完整流程从任务开始到交付约 2 小时 31 分，正式运行记录至报告校验约 2 小时 23 分；全量扫描约 1 分 34 秒，23 次完整计时工具调用合计约 18 分 42 秒。加上被中止的 Catalog 探查，工具时间约 26 分 45 秒，其余包含源码调查、解释、编排和返工，不能都算程序计算。

正常全量扫描私有峰值约 5.21 GiB、工作集约 6.01 GiB；补查各批私有峰值约 5.21–5.38 GiB。Catalog 异常查询观察到约 16.35 GiB 后主动中止，不是撞到 32 GiB 上限。没有完整逐秒进程树或 Viewer 峰值曲线，因此不能给出整个任务精确合计内存。

较早替代路线总跨度约 5 小时 36 分，成功扫描约 28 分 20 秒，其间包含工具故障、修复、编译和转场。两轮不是同版本受控性能基准，不能据此宣称算法稳定加速若干倍。[分析耗时与内存复盘.md:1](C:/Users/Admin/Documents/JN-Unity-T3/Analysis/T3-115721-cost-review-20261013/分析耗时与内存复盘.md:1) [近期聊天](appendices/conversations.zh-CN.md)

## 可复用的优化方向

- 工具与配置先预检，避免正式分析中途修工具。
- 将可兼容请求顺序批量执行，减少重复打开数据；对当前不支持的 Catalog 分支提前停止。
- 源码按 revision/文件/符号缓存，但每个原因仍保留独立证据关联。
- 一个 Viewer 会话批量导图，按视图保存定位、会话级复用交互验收。
- 报告增量组装并检查关键发现保留，减少反复全量导图和生成。

这些是基于耗时记录的后续方向。节省时间的估算不能写成已经测得收益；优化验收要保持候选、严重帧、统计与证据覆盖。

## 其他重要历史案例

| 案例 | 结果与交接意义 |
|---|---|
| Query R20 | 12.43 GB 冻结 Trace，复用 R14 索引；Scan 52 分 04.117 秒，总流程 74 分 12.541 秒。保留冷建恢复、跨进程账本、GPU Frame Evidence、Ghost 导出的旧缺口 |
| 自动登录 N22B | 修正 N20/N21 假阳性；登录与 60 稳定帧通过，但同轮数据全域质量失败 |
| Unity/Tracy Job 对账 | SRP Batcher 三层链在双方均存在；未见路径不能直接归因为没有插桩 |
| 最新 Client 补丁 | DLL/PDB 限定替换、5 秒实包后台录制、转换与索引结构错误 0；长时间前台仍待验证 |
| 固定切片 | 短流 CPU 与 Frame 投影阶段通过；最终全域/GUI/大源验收未完成 |

[Report.zh-CN.md:1](C:/Users/Admin/Documents/JN-Unity-T3/GpuCatalog-DiskIndex/Query-Final-Acceptance-20260929-r20/Report.zh-CN.md:1) [Player-T3-Auto-Login-and-Capture-Acceptance-2026-08-16.md:1](C:/Users/Admin/Documents/JN-Unity-T3/deployment-records/20260816/Player-T3-Auto-Login-and-Capture-Acceptance-2026-08-16.md:1) [SRPBatcher-Hierarchy-Audit.zh-CN.md:1](C:/Users/Admin/Documents/JN-Unity-T3/UnityProfiler-Tracy-JobWorker-20260928/SRPBatcher-Hierarchy-Audit.zh-CN.md:1) [deployment-result.json:1](C:/Users/Admin/Documents/JN-Unity-T3/artifacts/client-patch-20261012-115800-c0cde1b6/deployment-result.json:1) [README.md:1](C:/Users/Admin/Documents/JN-Unity-T3/Tracy-Fixed-Slice-Evidence/20261007-mcp-parity-001/README.md:1)

## 复现记录最小模板

```text
目的与验收范围
三仓提交 / dirty patch / 构建与 EXE SHA
输入 SHA / store schema / revision / 实际 capture config
命令 / 参数 / 环境 / 输出目录
功能结果 / 缺失域 / 数值与单位
工具耗时 / private / working set / 磁盘 / 系统余量
预期失败是否被正确拒绝
通过项 / 未完成项 / 下一步
```

新增代码影响统计或身份时，应包含一个可检测错误的负向样本；只比较两份正常输出相同，不能证明比较器能发现丢记录、改端点或错误关联。
