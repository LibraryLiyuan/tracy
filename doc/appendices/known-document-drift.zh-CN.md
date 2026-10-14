# 已知说明漂移与本次处理

当前手册已按指定分支说明；源码旁的旧 README、Skill 实现和机器配置没有在本次任务中改动。

| 旧位置 | 旧说明 | 当前依据 |
|---|---|---|
| 引擎 JNTracy README | ABI 1.7 | API 与 Package Runtime 均为 1.8 |
| Config README | 默认是 Daily 别名 | 默认 HighEvidence、Frame 引用、Catalog 关闭 |
| Capture GUI README | converter 必需、转换 .tracy | 当前依赖 stream-export 并发布 stream/store |
| Query README | 不建 SQLite/sidecar | 当前存在 EventStore、索引和 skeleton |
| MCP setup | 12 个工具 | 当前另有 tracy_scan，操作也含 triage |
| V2 README | 发布 2.3.0 | frontmatter 为 2.6.3 |
| ANALYZE.md | guard 4 GiB | mainline 源码为16 GiB；安装修订为32 GiB |
| Stream 视图参考 | 工具源在 profiler/stream-report-viewer | 当前源在 V2/tools/stream-report-viewer |
| 10 月 10 日整理 | dev、Skill2.3.1、2375包为当前 | 本次按 mainline、新三仓 SHA、V2和2453补丁更新 |
| 旧报告格式建议 | 仍在讨论 | V2 已增加原因与回查规范，旧生成器仍未自动落实全部合同 |

旧验收保留其事实与范围。说明更新不等于产品修复，也不自动关闭未完成验收。安装风险、追加窗口调查范围与报告证据保留问题见 [待办](../16-troubleshooting-and-open-items.zh-CN.md)。
