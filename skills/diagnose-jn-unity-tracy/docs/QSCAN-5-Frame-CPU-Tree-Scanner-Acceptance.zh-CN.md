# QSCAN-5 Frame / CPU Zone 树扫描器验收记录

## 1. 结论

状态：**Passed（阶段轻量门禁）**。

QSCAN-5 已实现基于 QSCAN-4 有界游标的完整 Frame 与 CPU Zone 树扫描器。扫描器生成 Exact 与 Logical 两套结构签名，按完整帧保存 `SignatureFrameRun`，并分别计算 inclusive、直接子区间并集和 exclusive。异常结构不会被静默截断或修正为“看似正常”的数值，而是降低对应事实的 exactness 并进入质量记录。

真实 `.tracy`、N30 Session 与 Profiler 的集中差分仍按总计划放在 QSCAN-13；本阶段使用独立 synthetic brute-force Oracle 验证算法。

## 2. 已实现范围

- 通过 `BoundedTraceScanner` 分页扫描 Frame 和 CPU Zone，批次上限由调用者指定。
- 按 FrameSet 只把具有合法 begin/end 的完整帧计入 `per_complete_frame` 分母。
- 为每个 CPU thread 保留跨批次开放 Zone stack。
- 支持 0～10 层及更深树；不会只统计固定层级。
- 直接子 Zone 即使重叠，也先做区间 union，再从父 inclusive 中扣除。
- Exact 签名包含 thread identity、完整 parent path 与 source site。
- Logical 签名使用 thread role、完整 parent path 与 source site。
- 同名 Zone 位于不同父路径时不会被错误合并。
- CPU 事实初步区分 `active_work`、`wait` 与 `intentional_pacing`。
- 每个签名同时输出完整帧分母和 when-present 分母。
- 缺 End、负区间、clock inversion、缺失父 Zone、未归属完整帧和不完整帧边界均进入 quality。
- 每个 frame run 保存有界代表事件引用，供后续 Candidate 与 AI 深查导航。

## 3. 正确性门禁

| 检查项 | 结果 |
|---|---|
| 父 inclusive 与逐纳秒 brute-force Oracle | 100% 一致 |
| 重叠直接子区间 union | 100% 一致 |
| 父 exclusive = inclusive - direct-child-union | 100% 一致 |
| 10 层嵌套全深度发现 | Passed |
| 父子关系跨 2 条记录的 scan chunk | Passed |
| 同名不同父路径 Exact/Logical 身份 | 保持独立 |
| `per_complete_frame` / `when_present` | 分别为 2 / 2（受控 Root fixture） |
| Missing End / negative / clock inversion | 明确失效，不进入精确耗时 |
| Missing parent | 明确报告 `missing_parent_zone` |
| 未归属完整帧 Zone | 明确报告，不污染完整帧分母 |
| 最大实际批次 | ≤2（受控分页 fixture） |
| 生产 `tracy-query.exe` 链接 | Passed |
| 当前完整 CTest | 13/13 Passed |

## 4. 测试过程

先建立测试目标但不提供扫描器头文件，构建按预期失败：

```text
error C1083: cannot open include file: TracyFrameCpuScanner.hpp
```

随后实现最小生产代码并执行：

```powershell
cmake --build build-query-scan --config Release \
  --target tracy-frame-cpu-scanner-tests tracy-query --parallel 1

ctest --test-dir build-query-scan -C Release --output-on-failure -j 1
```

结果：

```text
13/13 Passed
Total Test time: 6.06 sec
```

## 5. 构建身份

阶段基线：

```text
0b0af04e60525aa1ef75af4ba4eb9d6cf153c616
```

`tracy-query.exe` SHA-256：

```text
9D30344621D9FF6030C80EE8C81E4CB0814225358BBA28092A22DEACC640C49E
```

最终 `tracy-frame-cpu-scanner-tests.exe` SHA-256：

```text
C6A1DE91795F3518E5466E3D542292FD252C228282BE7D239637555310A3CEB6
```

## 6. 已知边界

- `CpuZoneDto.startNs` 是必填字段，因此“协议记录完全缺 Begin”应在 Protocol/Source quality 层被拒绝；本扫描器覆盖缺 End 和已声明 clock inversion。
- 当前阶段结果先以结构化内存 DTO 表达；QSCAN-8 会将 run 外排、精确排序并原子发布 Neutral Aggregate，以满足大型 Session 的内存门禁。
- Wait 分类在本阶段只形成候选事实，不在这里直接得出“性能问题”结论；QSCAN-9 才应用 Profile Policy。
- 捕获首尾的不完整 Frame 不进入否定性结论所用的完整帧分母。

## 7. 目标提交

```text
QSCAN-5 Add exact frame and CPU zone tree scanner
```
