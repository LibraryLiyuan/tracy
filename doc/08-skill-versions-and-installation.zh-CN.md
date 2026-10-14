# Skill 版本 工具配置与安装交接

本页的核心是把“技能文字规则”“本机配置”和“实际工具程序”绑定起来。仅把 SKILL.md 发给同事不足以复现流程。

## 目录与名称

| 对象 | 位置或名称 | 当前状态 |
|---|---|---|
| 旧仓库目录 | skills/diagnose-jn-unity-tracy | 2.3.1 |
| 新仓库目录 | skills/diagnose-jn-unity-tracyV2 | 2.6.3 |
| Skill 内部名称 | diagnose-jn-unity-tracy | V2 frontmatter 仍使用该名字 |
| 本机安装目录 | C:/Users/Admin/.codex/skills/diagnose-jn-unity-tracy | 2.6.3，带本机工具与策略配置 |

因此使用目录名区分源码版本，使用 frontmatter 名称识别技能。不要把 V2 目录名自动当成新的调用名称。[SKILL.md:1](../skills/diagnose-jn-unity-tracyV2/SKILL.md#L1) [SKILL.md:1](../skills/diagnose-jn-unity-tracy/SKILL.md#L1)

## 新旧版本的实质区别

| 方面 | 旧 2.3.1 | V2 2.6.3 |
|---|---|---|
| 输入路由 | 普通 .tracy 为核心；stream 先走采集/转换路由 | stream 直接走原生 analyze；普通 .tracy 保留旧路由 |
| 数值引擎 | Query 1.35 MCP 确定性 Scan | Stream 为 frame-triage-v4.1；旧输入仍用原合同 |
| 策略 | YAML、candidate-policy-v3 | Stream JSON profile 与 effective_policy；不声称使用旧 YAML module budgets |
| 调查覆盖 | 旧优先级映射、强制/批准队列 | 完整候选、全部严重帧、长 CPU/GPU 区间、全线程调查义务 |
| 原因分析 | 源码与证据合同 | 增加具体上游、Worker/完成端、缓存/预热/预算和反证要求 |
| 原生回查 | 旧规范与独立工具的部分集成 | Stream 有可执行网页内嵌 native renderer、逐问题截图和链接 |
| 验收 | Schema4 与旧验证器 | Stream 独立 audit/presentation 合同；不伪装为旧合同通过 |

V2 README 仍写 2.3.0，是说明漂移；本次以 SKILL.md frontmatter 和实际文件为准。[已知漂移](appendices/known-document-drift.zh-CN.md)

## 文件组成

完整目录包含 SKILL.md、references、scripts、schemas/assets、config 示例、bin 和 tools。Stream 的关键脚本是 stream_workflow.py、stream_review.py、stream_views.py；原生网页工具位于 tools/stream-report-viewer。[文件说明](appendices/skill-files.zh-CN.md)

当前 V2 与安装版有两个共同路径字节不同：bin/tracy-analyze.exe 与 config/.gitignore；安装版另有 6 个文件。其余 165 个共同文件一致。这是本轮哈希结果，不表示未来安装后仍自动一致。[清单](evidence/skill-inventory.json)

## 安装器实际行为

安装器将其自身父目录视为源码，目标名称固定为 diagnose-jn-unity-tracy；支持 DestinationRoot，已有目标需显式 Force。它使用 staging/backup 事务，保留旧 local-profile、分析 YAML，以及 stream-local、stream-profile、stream-viewer-local。[install-skill.ps1:1](../skills/diagnose-jn-unity-tracyV2/scripts/install-skill.ps1#L1)

**保留配置不等于保留旧 EXE。** 当前安装版配置期待 e707…32 GiB 分析器，而仓库 V2 带 49c7…旧分析器。直接 Force 安装可能复制旧程序却保留新哈希配置，导致身份核验失败，也可能回退可处理规模。

适合交接的顺序是：

1. 固定选择的源码目录与提交，记录完整 Skill 文件清单。
2. 在新的可写父目录试装，先检查目录结构、依赖和配置；不要把旧目录名误当目标名称。
3. 明确工具来源：重现 10 月 13 日案例用已记录修订版；按 mainline 交付则构建当前源码并重新验收。
4. 生成本机 stream-local 与 viewer-local，将路径和预期哈希绑定同一套程序。
5. 执行脚本 help、配置读取和小样本端到端检查，确认报告、图链和重启行为。
6. 验证后再替换正式使用目录，同时保留回滚材料和机器配置。

隔离安装示例：

```powershell
$skillSource = 'C:\CodeProjects\GodotProjects\tracy-0.13.1\skills\diagnose-jn-unity-tracyV2'
$reviewParent = Join-Path $env:TEMP 'JNTracy-Skill-Review'
& (Join-Path $skillSource 'scripts\install-skill.ps1') -DestinationRoot $reviewParent
```

输出应为 reviewParent 下的 diagnose-jn-unity-tracy 子目录。若该试装目录已存在，先核对其用途；不要盲目清理其他安装。

## 三份 Stream 配置

| 文件 | 内容与权威 |
|---|---|
| stream-local.json | executable 与 expected_sha256；显式命令参数优先 |
| stream-profile.json | 目标 FPS、显存阈值、代表预算、workers；用户显式参数优先 |
| stream-viewer-local.json | tool_root、profiler、profiler_sha256；绑定网页工具与原生 renderer |

当前仓库不含两份机器 local JSON，需要从示例并结合本机工具生成。配置中的用户目录和工具路径不是其他机器默认值。普通 .tracy 另有 local-profile.json 与 JNTracy.AnalysisProfile.yaml，不能用它们的旧示例显存值替代 Stream 用户阈值。

典型 local 内容：

```json
{"schema_version":1,"executable":"C:/YourTools/tracy-analyze.exe","expected_sha256":"填写已核验的64位SHA256"}
```

## 版本变动如何发布

修改规则、wrapper、生成器、工具接口或二进制时，同时更新当前说明和验证记录。一次分析只检查本轮产物，不重复整个 Skill 测试套件；修改或发布 Skill 时再运行相关脚本/工具回归。因果文字质量仍需要人工复核，不能只看 lint 或 schema 校验成功。
