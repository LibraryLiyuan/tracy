> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 当前证据缺口与文档后续建设项

> 首版：2026-09-07｜本文为文档中心的维护约定，不自动改变外部任务授权。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](source-index.md)

## 已知的开发阻塞与未核实事项

| ID | 事项 | 当前证据 | 后续应补的材料 |
|---|---|---|---|
| GAP01 | QSCAN14真实短Trace闭环 | FW5 run1/run2取消；Catalog-only与异常vector容量已修正 | 高基数聚合、序列化与恢复有界验证；第三轮真实闭环 |
| GAP02 | N30A新双路径等价 | P3缺响应、计数差异 | 字段/单位解释、Job底层核对、修复后独立差分 |
| GAP03 | GPU L0精确Frame归属 | 交接与设计已存在 | 实现提交、Main→Render传播、无Pass L0/多段/边界实测 |
| GAP04 | N31最终性能 | 计划和On/Off历史观测 | 成套源码/二进制、On/Off矩阵、成本窗口及语义一致性 |
| GAP05 | N28生产成熟度 | 三仓Schema3分支与计划存在 | 最终真实来源链/重连/乱序验收及当前合并结论 |
| GAP06 | DXR Player稳定性 | 历史成功、后续device removed、Editor成功 | 同源Player故障诊断与稳定性证据，不以Editor替代 |
| GAP07 | 新旧Skill入口一致性 | 安装1.1.3、草案2.0.1、旧wrapper缺新参数 | 配置迁移、正确启动入口、FW6安装发布证据 |
| GAP08 | GUI人工接受与Session GUI范围 | PreviewCandidate；后继Session GUI计划 | 当前tip人工交互及明确accepted/发布记录 |
| GAP09 | 全分支远端状态 | 本地refs快照，未fetch | 需要发布时再核对远端/PR，不从本页猜测 |
| GAP10 | 当前二进制兼容矩阵 | 文档合同与Git身份 | 实际每套EXE/DLL/PDB hash、--version/describe、同源构建manifest |

依据 [S10](source-index.md#s10) [S11](source-index.md#s11) [S22](source-index.md#s22) [S02](source-index.md#s02) [S05](source-index.md#s05) [S18](source-index.md#s18) [S04](source-index.md#s04) [S23](source-index.md#s23)。

## 文档首版的核对深度

已阅读主要设计、阶段结论和近期失败记录，检查重要脚本参数/正文入口，采样三个仓库本地分支和worktree。没有逐函数审计全部代码，也没有重跑历史测试；工具目录中仅盘点的文件已明确标注。

38份登记来源中，有些按重点章节阅读，有些仅作为相关计划/文件存在性入口。source-manifest的hash证明采样时文件身份，不代表完整正文逐条审计。未复制全部旧报告，也未把原始证据迁移到新目录。

## 后续文档建设顺序

1. 随QSCAN14、N30A修复更新真实状态和接口变化。
2. 对要交付的工具组合补成套二进制/运行合同。
3. 对关键链路增加对应具体commit的源码锚点；变更后维护。
4. N31、N28、OriginFrameId完结时补独立完整迭代记录。
5. 需要对外分享时，再基于审核版本生成Word/PDF，避免导出仍在变化的状态表作为正式结论。

本中心已覆盖系统、链路、迭代、分支、操作工具与协作；上述缺口是开发证据或进一步审计，不是空白文档占位。


