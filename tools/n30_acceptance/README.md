# N30A 确定性全量验收器

本工具执行已经批准的 N30A 验收设计。Python 仅编排正式工具、保存证据和比较数值，不解释 Tracy 协议，也不由 AI 决定通过与否。

## 运行

```powershell
python -m tools.n30_acceptance.run_acceptance --profile tools/n30_acceptance/profiles/n30a-full.json --inputs <local-inputs.json> --output <acceptance-root>
```

本机路径保存在独立 `local-inputs.json`；正式 Profile 和 Golden 随 Git 管理。P0 要求已跟踪工作区干净。P2 将工具编译到 `build-n30-acceptance-*`，随后锁定 EXE SHA-256。源码、输入、配置或二进制发生变化时不能复用原 Run。

## 查看结果和进度

每一个比较都打印：

```text
[COMPARE] name=... | rule=... | expected=... | actual=... | delta=... | status=PASS|FAIL
[COMPARE-SUMMARY][P5] total=... passed=... failed=...
```

- `comparisons.ndjson`：逐项结果，写入后 flush/fsync。
- `acceptance-progress.json`：验收阶段、当前命令和最近进程指标，每 5 秒更新。
- `raw/<编号>-<命令>/invocation.json`：启动前先落盘，便于识别中断命令。
- 同目录 `stdout.log`、`stderr.log`、`metrics.ndjson`：原始输出及每秒进程树采样。
- `report/`：机器结果、中文报告、JUnit、指标、重现命令和证据 SHA-256。

进度心跳证明验收器仍在工作；子进程的真实工作进展仍以 converter 日志、阶段、文件和进程指标判断。心跳不能代替转换进展。

## 阶段

P0 身份/容量 → P1 Python 自测 → P2 新工具链/CTest → P3 短 Trace 双路径 → P4 新 30 分钟转换 → P5 全量审计/Golden → P6 Query → P7 恢复 → P8 合成规模 → P9 报告。

`--rerun-phase P8` 等局部运行是开发诊断；缺少任何必需阶段时总结果为 `INCOMPLETE`。P3 失败后只允许既有 A7 的只读诊断，不能进入新的重型转换。产品失败保留证据并按设计阻断依赖阶段。

`--resume` 要求 Run 身份完全一致。已经发布的正式输入/Session 不被覆盖。P4 被中断后仍保留 building 目录；该目录不能被伪装成新的冷转换，后续恢复需要单独记录恢复证据。

## 合成语料

时间语料覆盖两小时及大于 32 位的 Frame ID，含 Zone/Job/Allocation/Resource/I/O 跨 Shard 生命周期。密度语料使用 1,000,000 / 2,000,000 / 4,000,000 个帧对，每个语义帧间隔 100 µs，使 1× 已跨多个时间 Shard；要求每组至少两个数据 Shard。

生成器每个协议压缩块独立编码，避免 LZ4 引用已经被覆盖的上一块缓冲。生成器报告的最大 Frame ID 必须与 Session 查询精确一致。

两小时逻辑跨度和密度测试分别执行；不将它们描述为两小时真实生产密度录制。

## 自测

```powershell
python -m unittest discover -s tools/n30_acceptance/tests -v
```

仅依赖 Python 标准库。Windows Job Object 的低预算测试只限制本测试启动的子进程，不修改系统配置。
