# N30A 验收器开发记录

日期：2026-09-07。分支：`feature/JNTracy-N30-LongTraceSessionStore`。

## 开发自测

当前 Python 完整自测 120 项通过，0 失败、0 错误。包含精确整数、Golden、日志、报告、局部运行不得宣称全量通过、进度落盘、取消、Windows Job Object 低内存限制、质量警告参与比较和依赖停止规则。

这些结果仅证明验收器自身，不能替代正式 N30A 验收。

## 已保留的合成开发运行

- `n30a-20260907-063208-58a9e670`：工具输出 schema 为十进制字符串，Python 解析器错误拒绝，记为 HARNESS_ERROR；已修正并加入测试。
- `n30a-20260907-063352-58a9e670`：68 个比较，67 PASS / 1 FAIL_PERFORMANCE。100k/200k/400k 均只有一个数据 Shard，未覆盖分片回收后的内存行为；4×/1× 峰值比约 2.764。保留原始失败，不据此修改门槛。
- `n30a-20260907-063723-6f45f123`：外部中断；最后记录位于 1× 派生索引，进程随后不存在，未形成阶段结论。

上述目录位于本机独立验收根目录的 `runs/`。未删除源文件、Session 或开发 scratch。

## 生成器修正

发现 LZ4 连续压缩引用了下一次会被重用的输入缓冲；改为独立压缩块。增加生成最大 Frame ID 与 Session 最大 Frame ID 的精确比较。密度基准提高到 1m/2m/4m，语义时间与 Journal 时间使用相同 100 µs 步长，要求至少两个数据 Shard。新语料需由独立构建产物正式验证。

## 正式运行前补齐

- 比较逐项输出 expected/actual/delta/status；原始日志纳入 SHA-256。
- 每 5 秒验收心跳；命令启动前保存 invocation。
- 锁定代码、工具、配置和输入身份；P9 重新检查 source SHA 与既有 Session 文件元数据。
- P5 重新执行只读 Canonical/Final Audit；Fresh 转换期已运行独立 GPU Verifier，其结果与 A7 和 Golden 比较。
- Converter 外层 18 GiB Job Object 护栏，独立于产品内部 16 GiB 限制。
- 局部阶段通过仍为 INCOMPLETE；依赖阻断不会误阻断允许的只读 A7 诊断。

正式 P0–P9 尚未完成，不应将本记录作为产品验收通过报告。
