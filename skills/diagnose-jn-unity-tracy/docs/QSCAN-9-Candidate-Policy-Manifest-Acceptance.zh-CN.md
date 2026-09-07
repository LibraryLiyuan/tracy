# QSCAN-9 Policy Evaluation 与 Candidate Manifest 验收记录

## 1. 结论

状态：**Passed（阶段轻量门禁）**。

QSCAN-9 已在 QSCAN-8 的 Neutral Aggregate 之上实现独立、确定性的 Policy Evaluation。Trace 统计事实与项目预算/调查策略保持分离：修改 Analysis Profile 只会重新生成 Policy identity、候选和 Manifest，不要求重新扫描 Trace。

本阶段只生成“需要 AI 深查的确定性候选”，不输出最终性能根因。候选由 Budget、Top、Anomaly、Capacity、Quality 和 UserFocus 六个相互独立的入口产生；相同结构问题合并为一个 Candidate Family，不计算容易掩盖证据的加权总分。

## 2. 六类候选池

| 候选池 | 输入 | 触发语义 | 默认优先级 |
|---|---|---|---|
| Budget | Frame/模块预算与精确 P95 | 仅 scope、marker rule 和预算定义均匹配时触发 | 固定预算 P1；规划预算 P2 |
| Top | Inclusive、Exclusive、Wait/Critical 全量排名 | Top N，并扩展到累计贡献阈值 | Top N P2；扩展项 P4；已证明关键路径 P1 |
| Anomaly | QSCAN-8 异常形态 | Isolated、Recurrent、Burst、Persistent | Isolated P3；其他 P2；关键路径 P1 |
| Capacity | CPU/GPU 精确峰值与资源预算 | value 大于已定义阈值 | 固定阈值 P1；暂定阈值 P2 |
| Quality | Neutral/domain audit | 域 invalid、未报告 gap 或审计不完整 | P0 |
| UserFocus | Profile 中的显式调查关键词 | 与 domain/signature/family/name/path 的规范化匹配 | P2，但属于强制调查项 |

未定义模块预算不会产生 Budget Candidate；模块预算的 `when_present` 与 `per_complete_frame` 不匹配时也不会错误触发。

## 3. 优先级与候选上限

- 排序固定为 P0 → P1 → P2 → P3 → P4，再按策略资格、Top rank 和 family ID 稳定排序。
- P0、P1 和 UserFocus 是强制调查项，不受 Profile 的可选 priority 列表或每域候选上限影响。
- `per_domain_limit` 只约束可选调查候选；强制项不会占用这 50 个名额。
- Top Policy 之外的非零排名仍作为候选进入 Investigation Backlog，保存 `outside_top_policy`，不会因候选上限从证据中消失。
- 所有 ranked signatures 都写入 Manifest；测试中的 60 条排名全部保留，其中 50 条 selected、10 条 backlog。

## 4. Candidate Family 与代表帧

- Family identity 由明确的 `familyId` 或 `domain + signatureId` 决定。
- 同一问题从 Top、Anomaly、Budget 等多个入口命中时合并为一个 Family，保留所有 member signatures 和 trigger evidence。
- 代表帧保存 frame index、选择理由和可导航 event refs。
- 当前确定性集合覆盖：前三个 worst、最接近 P95 的两个 typical_bad、first、last、每个新结构首次出现的 structure_change，以及最长 Burst 的 start/peak/end。
- 理由和 event refs 均排序去重；输入上下文逆序不改变输出内容 SHA。

## 5. 精确数值与 Manifest 契约

- 64 位字节数、事件总纳秒和计数以十进制字符串写入 trigger evidence，避免经 IEEE-754 `double` 后丢失整数精度。
- 浮点预算和统计值使用 classic locale 与 17 位有效数字的确定性表示。
- `candidate-manifest-v1.schema.json` 已覆盖实际输出的：
  - Aggregate/Profile/Policy/content identity；
  - 全量 ranked signatures；
  - structural/member signatures；
  - trigger evidence；
  - representative frame details；
  - quality status 与 backlog。
- 测试确认 GPU peak `7000000000` 和阈值 `6400000000` 原样写入，没有科学计数误差或末位变化。
- Manifest 先规范化序列化并计算 content SHA-256，再写临时文件并原子替换正式文件。

## 6. 输入防御

下列输入不会抛出未处理异常或产生不确定候选，而是返回明确 invalid：

- Aggregate/Profile/content identity 非法或 Profile SHA 不匹配。
- 缺失 Frame、模块、资源、候选或 UserFocus 所需的规范化字段。
- Frame/模块预算范围非法。
- Candidate priority 非 P0～P4 或重复。
- 同一 `domain + signatureId` 出现重复 Policy context。
- Neutral Aggregate 中出现重复签名。

拒绝重复签名尤其重要：此前使用 map 首项会让“输入顺序”隐式决定候选来源；现在该情况被当作上游数据错误显式阻断。

## 7. Synthetic 门禁

| 检查项 | 结果 |
|---|---|
| 六类候选池独立触发 | Passed |
| Parent/Child 多入口合并 | 一个 `hot-family`，成员 2，Top+Anomaly 均保留 |
| P0～P4 顺序 | 确定性非递减 |
| Scope mismatch | 不触发 Budget |
| 未定义预算 | 不触发 Budget |
| Top/Cumulative/每域上限 | 60 条均保留，50 selected，10 backlog |
| P0/P1/UserFocus 强制调查 | 禁用 P2 的 Profile 下 UserFocus 仍 selected |
| 代表帧 | worst/typical/first/last/burst/structure 全覆盖 |
| 输入逆序 | content SHA-256 完全一致 |
| Profile-only 修改 | Aggregate identity 不变，Policy identity 改变 |
| 64 位容量数值 | 精确十进制字符串一致 |
| Manifest 输出字段 | 全部由严格 Schema 声明 |
| 重复 Context/Aggregate | 明确 invalid |
| QSCAN-1～9 及既有 N29/N30 回归 | 17/17 Passed |

## 8. 测试记录

红灯阶段首先因 `TracyCandidatePolicy.hpp` 不存在而按预期编译失败；增加 Manifest 契约检查后，又按预期因输出字段未在 Schema 声明而失败。实现和 Schema 修正后执行：

```powershell
cmake --build build-query-scan --config Release `
  --target tracy-candidate-policy-tests -- /m:1

ctest --test-dir build-query-scan -C Release `
  --output-on-failure
```

最终结果：

```text
17/17 Passed
Total Test time: 7.48 sec
```

受控 QSCAN-9 测试二进制 SHA-256：

```text
A8FAD8791A7BFF1E81FDE650302A16522E654F8053648EAF83F2C29A94D1DDAF
```

## 9. 已知边界与后续门禁

- 本阶段 fixture 使用内存中的 Neutral Aggregate；持久 Scan、取消恢复、单 writer lease 和 MCP 分页属于 QSCAN-10。
- Candidate 是调查队列，不等价于最终根因；AI 必须在 QSCAN-11/12 继续获取精确关系、代表帧与源码证据。
- Profile 中暂定预算只能触发“规划参考”候选，报告不能将其写成已确认产品门槛。
- 单 Trace 不能证明 Tracy 相对无插桩包的 A/B 总体开销；这项边界必须保留到最终报告。
- 两份真实 `.tracy`、长 N30 Session 和 Profiler 人工对照集中在 QSCAN-13，不以本次 Synthetic 冒充。

## 10. 阶段身份

基线：

```text
f3be0971 QSCAN-8 Add exact statistics anomaly detection and neutral audit
```

目标提交：

```text
QSCAN-9 Add deterministic candidate policy evaluation
```
