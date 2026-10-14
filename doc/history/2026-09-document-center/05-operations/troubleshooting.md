> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 故障排查索引与已知问题

> 首版：2026-09-07｜命令与结论均需按适用版本阅读。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

| 症状 | 已知证据/原因边界 | 下一步检查 | 不能采用的捷径 |
|---|---|---|---|
| Render 阶段呈阶梯嵌套 | 粗 stage 跨越 deferred marker；已专项修复 | marker 边界分段、LIFO、Frame Root | 抑制真实 marker 丢信息 |
| GPU 两个 L0 不知属于哪帧 | 缺少 L0 自身精确 Frame 关系 | Main→Render ID 传播与 typed relation | 相邻时间或全局 currentFrame 猜测 |
| GPU Memory 窗口长期无结果 | G05 大型全量 Snapshot/working-set 合并 | 分支、输入规模、sidecar/Session 发布和 Builder 状态 | 把全量内存构建搬到转换期 |
| N30A 转换 exit0 但验收失败 | P3 Query 缺响应及45字段16差异 | 原始双路径响应、单位/字段、Job 底层计数 | 删除失败字段或宣布全部等价 |
| 333 MiB Trace 扫描很重 | FW5 run1/run2均触发保护；run2在精确统计阶段失败 | Catalog-only与空vector容量已修正；再查高基数聚合、序列化与恢复 | 提高上限、删域、减少 selected |
| Catalog On 明显变慢 | N31 指出重复生命周期/跨界事件与成本统计缺陷 | On/Off配套版本、未连接与已连接、closed-window成本 | 以关闭开关当完整修复 |
| DXR Player 0x887A0006 | 某次场景启动 device removed，根因未定 | 对应日志、DRED/Aftermath配置与同源复现 | Editor成功就说Player已修复 |
| Query 不能开始新扫描 | 可能为旧启动器/Schema/Profile 不匹配 | 1.35 tools/list、analysis roots、实际EXE SHA | 一次性CLI绕过合同 |
| Capture 有文件但不完整 | 未正常 drain/SessionEnd 或源中断 | stop回执、退出码、文件稳定和完整性 | 文件存在即Complete |
| 同名Marker“已查”仍无法通过 | 线程/完整父路径/Frame或请求响应不匹配 | 原始MCP回执与实体引用 | 拼接同名无关父链 |

来源按行对应 [S03](../08-references/source-index.md#s03) [S02](../08-references/source-index.md#s02) [S06](../08-references/source-index.md#s06) [S22](../08-references/source-index.md#s22) [S11](../08-references/source-index.md#s11) [S05](../08-references/source-index.md#s05) [S04](../08-references/source-index.md#s04) [S12](../08-references/source-index.md#s12) [S14](../08-references/source-index.md#s14) [S13](../08-references/source-index.md#s13)。

## 恢复记录应包含

本任务进程 PID+创建时间、EXE hash、输入强身份、最后持久化状态、取消请求/响应、已完成阶段、正在写入的产物和下一步允许动作。PID 只是历史编号，恢复前必须重新验证归属，不按旧报告 PID 终止当前进程。

先保存失败，再最小化复现和验证假设。已经失败的生产门禁保留 Failed/Incomplete；轻量测试通过只升级对应测试层级，不覆盖失败真实运行。

