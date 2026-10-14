> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 链路 01：采集、Journal 与正常结束

> 首版：2026-09-07｜用途：开发知识与跨对话协作｜事实以标注来源和版本为限。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 主路径

```text
有效配置与连接状态
→ 各 producer 产生定义、事件、关系和质量计数
→ Client 缓冲/协议传输
→ Capture 连接目标
→ .tracy-stream 增量写入
→ stop request
→ disconnect + drain
→ SessionEnd + 文件关闭 + Capture 退出
→ 完整性检查
```

新采集应记录目标、场景、Editor/Player、图形 API、gfx-jobs、画质与分辨率。请求 HighEvidence 后仍须用 Trace AppInfo 的 effective config 确認实际生效档位。ready marker/真实场景就绪是开始采集的依据，固定等待秒数不能替代。[S14](../08-references/source-index.md#s14)

## 关键合同

| 阶段 | 输入/操作 | 应保留的证据 | 失败处理 |
|---|---|---|---|
| 准备 | EXE、地址、端口、独立任务目录、保护阈值 | 配置、文件身份、ready 证据 | 缺失时不猜路径或自动就绪 |
| 启动 | Capture -j stream，stop-file 路径 | PID+创建时间、stdout/stderr、stream 增长 | 检查连接和目标进程 |
| 运行 | 监控 stream、工作集、磁盘和日志 | 时间戳与采样记录 | 达阈值请求正常停止 |
| 停止 | stop-file 或正确控制台一次 Ctrl+C | 停止来源、退出码、排空日志 | 等待 Client/定义/事件排空 |
| 发布 | SessionEnd、正常退出、文件稳定 | Complete/RecoverablePrefix/Corrupt 状态 | 不能只按文件存在判定完成 |

正式实时采集只写 stream；离线生成 .tracy。历史 Full 双写会增加运行时负担，N25.6 转向 stream-only 和主动 drain。[S16](../08-references/source-index.md#s16) [S14](../08-references/source-index.md#s14)

## 常见失效点

- stop-file 在启动前已经存在：会造成意外停止，启动前核对独立任务路径。
- 第二次 Ctrl+C、关闭窗口或终止进程：不能标记正常完整采集。
- 先退出 Player：可能使排空证据不完整；正常停止期间保持目标存活。
- 有 Viewer/Capture 已连接：新 Capture 不能假设自己拥有连接。
- 管理员权限具备但 Sampling/Context Switch 无数据：仍以 Trace capability 为准。
- 文件短不代表事件少，保护阈值应涵盖后续离线 Query 的资源规模。

## 操作与维护入口

现有脚本 [start-capture.ps1](C:/Users/Admin/Documents/JN-Unity-T3/skill-draft/diagnose-jn-unity-tracy/scripts/start-capture.ps1)、[monitor-capture.ps1](C:/Users/Admin/Documents/JN-Unity-T3/skill-draft/diagnose-jn-unity-tracy/scripts/monitor-capture.ps1)、[stop-capture.ps1](C:/Users/Admin/Documents/JN-Unity-T3/skill-draft/diagnose-jn-unity-tracy/scripts/stop-capture.ps1) 使用 resolved JSON profile，不能把新 YAML 原样传给它们。参数及兼容说明见 [工具目录](../05-operations/tool-catalog.md)。脚本入口已静态阅读，本次未启动采集。[S30](../08-references/source-index.md#s30) [S31](../08-references/source-index.md#s31) [S32](../08-references/source-index.md#s32)
