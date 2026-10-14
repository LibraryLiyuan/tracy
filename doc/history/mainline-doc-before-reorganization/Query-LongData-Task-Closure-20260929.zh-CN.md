> 历史归档：mainline f49190f2541c 中的历史资料。当前版本与操作入口见 [文档首页](../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# Query 长数据任务收尾与后续交接

日期：2026-09-29。用户随后决定撤回本地dev合并，让Query在功能分支继续完成，再一次性合入dev。当前本地`dev`已退回`b840ae76`且Git状态为空；此文记录原批次成果和当时合并验证，不能作为dev现已包含Query的证明。性能优化在新的对话继续，原始全部验收要求仍未完成。

## 交付基线

- R19冻结源码：`ea6ebfbe641828e36a3398172e883a7930760237`，内部索引schema49，对外Query1.35。
- 合并前dev：`b840ae76ecafad327b65edeaf0d2252475e71870`，包含Capture GUI和Local Report Viewer。
- 输入：30分钟录制，12,428,310,213 B，SHA256 `da845bbc88e15688f262f979529e5904fcfd560b4ff0936cd8b6e525c3c5935a`。
- R19 EXE SHA256：`5ed5d16ac20e254386c13d09e6a9d6624c8c727f47441d2880b0659bdc5e9858`。该冻结二进制与合并后的构建身份不同，历史结果不能标成合并版重跑结果。
- 包含磁盘事实表与索引、共享资源计账、JNTRACYX v1读取/转换支持、Catalog质量校验、Job/Sample/GPU关联消费、Native Scan及阶段恢复相关更改。

## 已有真实验证

| 项目 | 已验证结果 |
|---|---|
| R14冷建及命令内深校验 | 58分03.567秒；索引40,324,284,637 B；该轮账本峰值80,449,152,815 B |
| R19 Scan | Native阶段59分01.484秒；包含首次加载的脚本59分34.523秒；正常退出0 |
| R19总时钟 | 100分42.975秒，不含历史R14冷建，包含客户端修复及接续准备间隔 |
| 热查询及分页 | 1,000次点查；2,528,691 Resource、94,168 Candidate、66,283 Signature完整分页 |
| 关联 | 全量后端校验复用；1,000 Pass双向分页、32 Resource窗口通过 |
| Ghost | 945,327,458条后端校验，8个MCP窗口；未执行全量MCP导出 |
| Profile重算 | 15分30.944秒；复用同一Neutral聚合内容SHA |
| 全分析最终派生盘 | 包含历史保留目录共46,172,826,297 B，低于4S=49,713,240,852 B |
| Private Bytes峰值 | R14冷建7,913,672,704 B；R19 Scan3,401,097,216 B；Profile3,592,888,320 B |

原热验收脚本因签名审计键只使用signature_id而失败；Query身份实际为(domain, signature_id, frame_scope)。4项红绿回归后独立接续通过。原始失败和冻结脚本保留；下次使用的验收脚本必须吸收三元组修复与抽样状态持久化。

## 未关闭项与使用边界

1. 性能优化：Scan门禁余量不足一分钟；CPU遍历约25分钟、Policy约15分钟、统计归并/reduce约8–9分钟是下一轮重点。冷建GPU Reference计时同时含构建和随后校验，应先拆分再定位。
2. R19缺少所有复用根目录与临时文件的连续磁盘峰值证据；最终量通过不代表8S峰值已经证明。
3. 冷建检查点与Ghost ordinal续页已实现并有小型真实后端回归；本次大Trace中断恢复和完整Ghost MCP分页仍未验收。
4. GPU Frame Evidence仍明确unsupported。Catalog Core有效，但enrichment有未解析记录；原CPU/GPU等捕获质量invalid仍保留，不能解释为全域健康。
5. 主机系统commit余量曾低至379,289,600 B；新增重型任务前应核对主机余量，不通过提高32 GiB硬限制或4 GiB共享workspace掩盖压力。
6. 仅验证此30分钟录制，不外推60/120分钟。120分钟接续门禁不含历史冷建。

当前资源合同：Private Bytes软目标16 GiB、硬限制32 GiB、共享workspace4 GiB、最终4S、存活峰值8S。后续不把未执行要求改写成通过，不默认重跑完整Trace。

## 合并冲突处理

- Capture CMake：同时保留存储/转换测试和dev的Windows UTF-8 manifests。
- stream-convert：保留受预算控制的RunConversion/清理/诊断流程，在最外层入口接入dev的UTF-8 argv转换。
- vendor：同时保留显式prepatched模式与dev的本地依赖免重复patch逻辑。
- TracyVector：保留分配失败诊断及bad_alloc传播；替代旧dev的abort路径，使Query/Converter可按资源错误处理。

## 后续对话入口

Query性能优化从`feature/JNTracy-GpuCatalog-DiskIndex`当前HEAD建立独立工作树；并行的其他功能从合并前`dev`提交`b840ae76`另建分支。当前功能工作树保留用于继续Query，不自动清理或归档。历史整合提交`e13eefa3`仍由Query功能分支保存，本地dev不再指向它。

完整本机证据根目录：`C:\Users\Admin\Documents\JN-Unity-T3\GpuCatalog-DiskIndex\Query-Final-Acceptance-20260929-r19`，包含`Report.zh-CN.md`及`results/final-independent-audit.json`。收尾构建/回归日志位于同级`Task-Closure-20260929`。大型Trace、索引、EXE/PDB和日志不纳入Git。

## 收尾验证

- 合并树Query Release（开发no-LTO配置）完整构建通过，CTest **61/61**通过，202.88秒。
- Capture/Stream Converter Release构建通过，转换器命令行入口检查通过；Tracy自身CTest **32/32**通过，10.52秒。
- 首次Converter CTest受到9月17日遗留的四个测试夹具目录影响：已有文件被创建保护拒绝，四项失败。相同二进制在四个全新目录中分别通过；将旧夹具移至收尾证据目录保留后，32项Tracy测试全部通过。另有未构建的第三方Zstd fullbench/fuzzer/zstreamtest及禁用的playTests，不算作本项目32项回归；首次失败日志保留。
- 生产代码未因测试失败而放宽覆盖保护。遗留测试的固定输出目录需要在重跑前隔离，不能把这类环境失败视为Query OOM。
- 原生Profiler Release目标构建通过；未重跑浏览器/GUI端到端验收。合并验证不重跑12.43 GB冷建或Native Scan，不将历史R19结果标为新合并EXE的性能测量。
