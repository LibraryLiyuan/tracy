# Query MCP 和 tracy-analyze 使用与接口

Query 提供结构化事实与通用查询，MCP 是其模型侧接口，tracy-analyze 是当前 Stream 原生候选与取证 CLI。MCP 中的 tracy_analyze 工具名称与 tracy-analyze.exe 不代表同一工作流。

## 入口选择

| 目的 | 入口 |
|---|---|
| 普通 .tracy 确定性扫描和旧 Skill | Query 1.35 MCP + tracy_scan + YAML Profile |
| 已发布 store 的 Stream 完整候选分析 | tracy-analyze --mode diagnose，或 Skill stream_workflow.py |
| 快速看帧和显存样本 | tracy-analyze --mode shortlist；不能回答完整 CPU/GPU 受限判断 |
| 定向查事件树、资源、I/O、栈或图像 | Query 请求/MCP inspect，或 analyze 的 requests plan |
| 采集结束状态和能力核对 | trace status/info、describe、quality 及实际 capture-context |

## Query CLI 和 Session

```powershell
tracy-query.exe --version
tracy-query.exe --schema
tracy-query.exe --doctor --trace C:\YourCaptures\capture.tracy-stream
tracy-query.exe --trace C:\YourCaptures\capture.tracy --request request.json
tracy-query.exe --trace C:\YourCaptures\capture.tracy --batch requests.ndjson
tracy-query.exe --mcp --allow-root C:\YourCaptures --allow-source-root C:\YourSource
```

这些是程序入口示例，具体运行先确认 EXE 身份。MCP 采用一行一个 UTF-8 JSON-RPC，stdout 只传协议，诊断写 stderr。open 异步返回 session，必须轮询 ready；默认最多保留两个活跃 session，加载按实现串行。[TracySessionManager.cpp:81](../query/src/TracySessionManager.cpp#L81) [TracyMcpServer.cpp:1](../query/src/TracyMcpServer.cpp#L1)

路径先 canonicalize，再检查 allow-root。外部源码读取有独立 allow-source-root；不要通过更宽权限来掩盖错误输入路径。结束后显式 close，不能只丢弃客户端引用。

## MCP 工作流

当前工具集中保留 open/status/close、describe、overview、search、inspect、timeline、analyze、compare、validate、job，并增加了 tracy_scan。旧 setup 文档中的“12 个工具”已经落后；实际以本轮 tools/list 和 describe 为准。

普通顺序为：open → status ready → describe/info/quality → scan 或定向查询 → 完整分页 → 回执落盘 → close。对确切 Query 方法，tracy_inspect 可使用 method、trace_id、params；参数未知时先 describe 对应方法，不猜字段。

```json
{
  "method": "plot.list",
  "trace_id": "trace-1",
  "params": {"limit": 100}
}
```

上例 trace_id 必须替换成实际返回值。较新的 tracy_scan 路由含 analysis.scan.triage；不要把新方法的存在解释成旧 Skill 全部状态/算法自动变化。[TracyMcpServer.cpp:1](../query/src/TracyMcpServer.cpp#L1)

## analyze 模式与参数

```powershell
tracy-analyze.exe --trace C:\YourCaptures\capture.tracy-stream `
  --output-dir C:\YourAnalysis\run-001 --mode diagnose `
  --fps 60 --vram-limit-bytes 5368709120 --max-representatives 64 --workers 4
```

CLI 默认 triage，默认代表数量 24；Skill profile 默认代表数量 64。shortlist 只读帧和已记录显存，triage 做候选，diagnose 还读代表窗口。输出目录必须新建或为空；当前 schema store 不可用时拒绝。[analyze.cpp:249](../query/src/analyze.cpp#L249) [TracyAnalyzeFast.hpp:1](../query/src/TracyAnalyzeFast.hpp#L1) [stream-profile.json:1](../skills/diagnose-jn-unity-tracyV2/config/stream-profile.json#L1)

| 参数或字段 | 含义 |
|---|---|
| --fps | 帧目标；保存到 effective_policy |
| --vram-limit-bytes | 被测程序显存阈值，5 GB 与 5 GiB 分开记录 |
| --max-representatives | 代表取证预算，不能截断全部候选或严重帧义务 |
| --workers | 分析并发数量，CLI 接受 1 至 32 |
| --plan | 明确的窗口与 Query requests，每批最多 64 窗口 |
| --skip-triage | 对已知窗口追加证据，不重新全量筛选 |
| --cache-dir / --no-cache | 同身份阶段缓存的位置或禁用复用 |
| --resume | 将已验证完整 triage 阶段恢复到新输出，不是跨进程恢复旧游标 |

mainline 进程私有内存 guard 固定 16 GiB，不能通过显存阈值参数改变。32 GiB 安装程序有独立来源记录；旧 ANALYZE.md 中的 4 GiB 说明已过时。[analyze.cpp:296](../query/src/analyze.cpp#L296)

## 数据与完成语义

FrameTriage 对已观测层级划分不重叠 self 区间，保留 work、wait、unknown 等独立信号。角色可能有多个 owner，其总量存在并发；不能相加为一个线程的 on-CPU 时间。GPU union/root span 也是信号，不自动等于帧关键路径。[TracyFrameTriage.cpp:1](../analysis/TracyFrameTriage.cpp#L1)

显存样本采用有界的最近观测保持，并记录最大年龄；无法证明未采样的真实峰值。incomplete/invalid scope、缺失域与候选完整性各自保存。recorded_candidate_enumeration_complete、all_domain_coverage_complete、causal_classification_complete、source clean end 应分别解释。

## 分页 预算和错误

常规 next_cursor 分页只在同一 session/revision 中有效；重复、过期游标、partial 或超预算必须保留缺口。analyze Query plan 每请求最多 1,000 页、256 MiB 回执；退出 0 表示执行成功，2 表示补证不完整，1 表示错误。退出 0 仍不替代各域与因果状态审查。

EventStore page cache、结果缓冲、analysis guard、磁盘预算与系统 commit 余量是不同层次。发生 RESOURCE_LIMIT 时记录具体预算名称，再选择缩小单次补查、分页、重建或经过授权的实现调整；不要将失败转成零值。

## 回执至少保存什么

方法和规范化参数、请求/响应身份、输入 SHA/revision、EXE SHA、策略/算法版本、耗时、游标、返回范围、partial/truncated/warnings、输出文件 SHA。时间单位明确到 ns/ms，次数与成本不能互换。

已经确认 Catalog 不存在时应在能力检查后跳过昂贵的 Catalog 验证分支。历史完整分析曾在这一步观察到 16.35 GiB 私有内存并主动终止；这不是所有录制都会出现的固定成本。[真实案例](14-validation-and-real-cases.zh-CN.md)
