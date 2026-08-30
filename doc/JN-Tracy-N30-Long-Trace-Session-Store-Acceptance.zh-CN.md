# JN Tracy N30 长时间 Trace Session Store 验收记录

## 1. N30.0 基线

记录时间：2026-08-30。

### Git

```text
Repository: C:\CodeProjects\GodotProjects\tracy-0.13.1-n30-long-trace-session
Branch:     feature/JNTracy-N30-LongTraceSessionStore
Base HEAD:  15ceef123bed1ae0a87ed0cb4af908c3b19e83e8
Subject:    Implement N29 GPU resource analysis sidecar
```

N30 从 N29 独立 worktree 开始；不得修改或合并 `dev`、N29 worktree 和其他现有 worktree。

### N29 工具基线

| 工具 | 大小 | SHA-256 |
|---|---:|---|
| `build-n29-capture\Release\tracy-stream-convert.exe` | 11,300,352 | `7930C68F01B5CDFAB2C7990A4A9EC08FC9108B34076BF8A7A48AD6C4E5407214` |
| `build-n29-query\Release\tracy-query.exe` | 15,267,840 | `D40A151ADA7FFA8CCCCBB286E5F94875BACF1BF9DED43FC7BA7E1143C99A37BC` |
| `build-n29-query\Release\tracy-gpu-analysis-build.exe` | 11,512,832 | `1F38C058F7FB9BEC956960AD0B764B6980016279883386FCB356D1442CB1F76B` |
| `build-n29-profiler\Release\tracy-profiler.exe` | 27,313,152 | `5A6CA7B43B6A041B88C7183D2D37ADB54F2FC05BCF5320728074E13C2A35E149` |

版本基线：Protocol 90、JN section 12、Query API 1.33、N29 GPU Analysis schema 1。

### 短 Trace Oracle

```text
C:\Users\Admin\Documents\JN-Unity-T3\N29-Autonomous-Smoke\Smoke-20260830-214952
```

已知结果：

- 90秒自动Player流程成功。
- Stream：1,235,094,582 bytes。
- Trace：616,456,038 bytes。
- N29 GPU sidecar：4,530,300,324 bytes。
- GPU Resource records：1,498,592。
- GPU Allocation records：495,993。
- GPU Passes：1,969,665。
- GPU Uses：26,533,453。
- EngineKnownPhysical peak：4,140,236,800 bytes。
- FrameImage：75张，960×540，间隔60帧。

该数据用于传统Full Worker、N29 standalone sidecar和N30 Session之间的差分Oracle。

### 真实30分钟压力输入

```text
Path:
C:\Users\Admin\Documents\JN-Unity-T3\N29-Autonomous-30m\Run-20260830-220012\Admin-HighEvidence-30m.tracy-stream

Size:
19,122,822,908 bytes

SHA-256:
2AC46C53257CE9027EC67E098FC15070FB911243F6CD311A166EB97547848068
```

N29转换基线：

```text
DecodeBuild progress: 42.6%
Process commit:        40.83 GiB
Failure:               system memory pressure exceeded the offline conversion safety limit
Projected full replay: approximately 90–100 GiB
```

该失败发生在N29 GPU Analysis Builder开始之前，证明N29解决GPU分析读取问题，但未解决完整Worker replay的总量内存问题。

## 2. 阶段状态

| 阶段 | 状态 | 结果 |
|---|---|---|
| N30.0 隔离、计划与Oracle | Passed | 独立分支/worktree、正式计划、Oracle和30分钟失败基线已提交。 |
| N30.1 Schema与原子存储 | Passed | Schema 1、强身份、Shard校验、多generation原子发布和查询门禁通过。 |
| N30.2 Inventory与容量预检 | NotStarted | — |
| N30.3 Canonical/Checkpoint | NotStarted | — |
| N30.4 全Canonical域 | NotStarted | — |
| N30.5 Derived/N29整合 | NotStarted | — |
| N30.6 Query/MCP/导出 | NotStarted | — |
| N30.7 LTS-1 | NotStarted | — |
| N30.8 Profiler Session | NotStarted | — |
| N30.9 LTS-2 | NotStarted | — |

后续每阶段记录：源码HEAD、测试RED/GREEN证据、工具SHA-256、输入身份、事件/关系计数、内存/磁盘/耗时、失败项和回滚提交。

## 3. N30.1 Schema 与原子存储

### 实现结果

- 新增 Trace Session Store、Canonical 和 Derived schema 1。
- 新增完整 Session 状态模型、Source Identity、Manifest 和 Shard 描述。
- Shard 使用相对路径、大小和 SHA-256 校验；拒绝目录逃逸路径。
- `.building.<uuid>` 在发布前不可作为 Trace 查询。
- 首次发布把 building generation 原子切换为最终 Session。
- 相同 source identity 可发布后续 immutable generation，并原子更新 `CURRENT`；上一成功 generation 保留。
- 不同 source identity 拒绝覆盖现有 Session。
- 完成 Session 可在原 stream 暂时不可用时继续查询；需要重建时通过独立的完整 SHA-256 校验 source identity。
- 同尺寸原地篡改 Shard 可被 SHA-256 校验识别。

### TDD 证据

RED：

1. 首次测试因 `TracyTraceSessionStore.hpp` 尚不存在而编译失败。
2. 增加第二 generation 用例后，旧发布逻辑返回 `session_output_exists`，用例按预期失败。
3. 增加强 source identity 用例后，因 `VerifyTraceSessionSourceIdentity` 尚不存在而编译失败。

GREEN：

```text
Test project C:/CodeProjects/GodotProjects/tracy-0.13.1-n30-long-trace-session/build-n30-query
    tracy-gpu-analysis             Passed
    tracy-trace-session-store      Passed
100% tests passed, 0 tests failed out of 2
```

覆盖用例：

- schema 常量与默认目录命名。
- building 状态查询拒绝。
- Canonical Frame Shard 写入和 SHA-256 校验。
- 强制 Derived/Audit 未完成时禁止发布。
- 首次发布、读取和完整校验。
- 相同 source 的第二 generation 切换与旧 generation 保留。
- 相同大小 source 替换的强身份不匹配。
- 相同大小 Shard 内容篡改。
- 既有 N29 GPU Analysis 回归。

### 构建环境诊断

首次配置使用新 build 目录自行下载 Capstone，Windows 路径长度超限。对照 N29 的 CMakeCache 后确认根因是遗漏已有 CPM source cache。重新配置为：

```text
CPM_SOURCE_CACHE=C:\CodeProjects\GodotProjects\tracy-0.13.1-n29-gpu-analysis\build-n29-deps
```

之后配置、编译和测试正常；没有修改或复制 N29 的依赖缓存。

### 延后至 N30.3 的持久性项目

- 当前文件提交具备临时文件、校验和与原子 rename；完整故障注入和恢复矩阵在 N30.3 执行。
- `FlushFileBuffers` 级耐久性、孤立 generation 清理和杀进程恢复在 Checkpoint/Recovery 实现时统一完成。
