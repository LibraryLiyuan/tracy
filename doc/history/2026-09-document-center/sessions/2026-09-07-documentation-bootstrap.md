> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 文档建设记录：2026-09-07

> 首版：2026-09-07｜本文为文档中心的维护约定，不自动改变外部任务授权。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 目标

按用户确认的文档体系，在 C:/Users/Admin/Documents/JN-Unity-T3 内生成系统、关键链路、开发迭代、分支与操作工具文档，并补齐指标、验收、决策和跨对话维护入口。

## 实际动作

- 阅读现有本地计划、交接、验收、失败报告及关键脚本接口。
- 只读采样jnunity、PackageRepo、Tracy的本地分支、worktree、merge-base、commit摘要与tracked状态。
- 发现仓库所有权提示后采用命令级safe.directory，不改全局Git配置。
- CodeGraph用于已索引Tracy仓库初步定位；不依赖被标记变动的大文件旧索引认证源码。
- 建立JNTracy-Docs及根目录导航，按主题组织知识并链接原始来源。
- 保存来源清单、SHA、分支快照和文档校验结果。

## 重要发现

安装Skill为1.1.3，草案为2.0.1；旧设计的1.1.2标签已不是现场安装值。QueryScan工作区有24项tracked改动；旧HEAD不足以表示QSCAN14状态。新MCP合同与旧启动wrapper有参数差异。

N30A旧A7与新P3属于不同运行/门禁；QSCAN13和QSCAN14的调查规则不同；Render CPU嵌套、GPU帧归属与DXR计时是不同专题。

## 变更范围与验证

只新增文档中心及根目录导航、来源/快照/校验资料；没有修改原报告、源码仓库、Skill、Player或配置，没有构建/启动游戏/采集/转换/重跑分析，也没有提交或发布。

实际文档数量、链接检查、来源漂移和文件清单见 [document-validation.json](../evidence/document-validation.json)。该记录仅证明文档检查，不代表产品验收。

## 后续入口

新对话先读README和STATUS，再选择专题。当前首要开发跟进材料见QSCAN14、N30A与known-gaps；本次不替其他任务继续其实现或验收。

## 收尾复核中的并行变化

来源hash检查发现S10/S13/S15/S27/S36被其他任务更新；已重读相关内容并加入S38的run2报告。状态表区分两轮失败，补记空vector修正，未当作第三轮真实验收通过。Git图确认GUI tip已被本地dev包含。初始来源hash保存在source-manifest-initial.json，复核记录在source-review.json。
