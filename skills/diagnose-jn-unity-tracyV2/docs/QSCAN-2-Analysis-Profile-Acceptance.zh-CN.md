# QSCAN-2 Analysis Profile 与安全路径阶段验收

日期：2026-09-06  
状态：Passed  
Tracy 分支：`feature/JNTracy-AI-QueryScan`  
阶段提交：`b00344a7`（`QSCAN-2 Add strict external analysis profile contract`）

## 1. 本阶段结果

- Query 1.35 已实现 `analysis.scan.profile.validate`，由 C++ 负责严格字段验证、单位换算、规范化和 Profile SHA-256。
- `6.4 GB` 固定按十进制解析为 `6,400,000,000 bytes`。
- `16 GB` 暂定进程内存阈值解析为 `16,000,000,000 bytes`，并保留 `provisional`。
- 模块预算统一标为 `planning_reference`，不能冒充产品硬预算。
- GPU Pass 预算保持未定义；Profile 中添加该字段会被严格拒绝，不能形成伪 Budget Candidate。
- Query 新增成对的 `--analysis-root` 与 `--analysis-cache-root` 参数。两者必须为绝对路径，analysis root 必须已存在，cache root 必须位于其中。
- Skill 草案使用 vendored PyYAML 6.0.2 的纯 Python `SafeLoader`；危险 Python object tag 已实测拒绝，运行时不安装依赖、不访问网络。
- Query 是规范化和校验权威；Python 只读取 YAML、调用 Query，并原子写入审计快照。

旧 `performance-budgets.json` 与 `marker-attribution.json` 仅为 Skill 1.1.2 回归测试保留，已标记 `legacy_compatibility_only`；正式新流程的唯一权威为 `JNTracy.AnalysisProfile.yaml`，旧路径在 QSCAN-11 从正式路由移除。

## 2. 验收结果

| 门禁 | 结果 |
|---|---|
| C++ Debug CTest | 10/10 Passed，9.26 秒 |
| Skill Python unittest | 20/20 Passed，2.04 秒 |
| 危险 YAML tag | Passed，明确抛出 `ProfileResolutionError` |
| YAML → Query → normalized JSON/YAML | Passed |
| Query/Profile schema 镜像 SHA-256 | 完全一致 |
| cache root 逃逸 | Passed，构造阶段拒绝 |
| 不存在的合法 cache 子目录 | Passed，不要求预先创建 |
| GPU Pass 未定义预算 | Passed，规范化结果无该字段 |

集成验证得到：

```text
profile_identity = 558d6dc1187ec40d934569127af25208895f6b577e798d7376eb4f473ba3b624
gpu_memory_bytes = 6400000000
cpu_memory_bytes = 16000000000
module_budget_count = 20
gpu_pass_budget_defined = false
```

审计输出保存在：

```text
C:\Users\Admin\Documents\JN-Unity-T3\Analysis\QSCAN2-profile-integration
```

## 3. 关键身份

```text
tracy-query.exe
29AE3C1EDF2348B19600172BFBE61F4B830CBB2F9ACFEDB27E683FDE121C09EB

Query analysis-profile-v1.schema.json
FD0E0FC070FAC3FDEDED7708009B2B0A47E36436A8BF6AF1296C132798A9FA51

Skill analysis-profile.schema.json
FD0E0FC070FAC3FDEDED7708009B2B0A47E36436A8BF6AF1296C132798A9FA51

JNTracy.AnalysisProfile.yaml
895E2FD4B4543971AD45D903E34FAF07E7C2D504D6789AF6938C523D1F084F6D

resolve-analysis-profile.py
2B0A7C69CCCD3593373B24C63799D397AAF4C330886968EDB7D75CC149EA0DB2
```

注意：上面的 Query EXE 是 Debug 阶段产物；正式安装身份在 QSCAN-13 Release 集中构建后重新记录。

## 4. 未进入本阶段的内容

- 尚未启动真实 Trace 全量扫描。
- 尚未写 Neutral Aggregate。
- 尚未生成候选或报告。
- 尚未替换已安装 Skill。
- Protocol 90、JN section 12 和 Session schema 均未改变。

下一阶段为 QSCAN-3：Neutral Aggregate 存储、强身份、校验和、原子发布与恢复。
