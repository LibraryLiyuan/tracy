> 历史归档：2026 年 9 月文档中心快照。当前版本与操作入口见 [文档首页](../../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 链路 07：三仓构建、Player 部署与交付

> 首版：2026-09-07｜用途：开发知识与跨对话协作｜事实以标注来源和版本为限。
> [返回文档入口](../README.md) · [当前状态](../STATUS.md) · [来源索引](../08-references/source-index.md)

## 版本到运行的证据链

```text
三仓分支/commit + dirty patch
→ 配套 ABI / Protocol / Query 合同
→ 独立构建目录
→ EXE / DLL / PDB hash
→ 完整独立 Player 副本及部署 manifest
→ 静态 Validate
→ 目标场景 ready
→ 真实采集与 Query 校验
→ 限定范围的交付结论
```

旧部署指南记录了三仓构建和同事验收。实际操作应使用对应专题分支与合同，不能把旧固定 hash 配上任意最新 EXE。[S01](../08-references/source-index.md#s01) [S25](../08-references/source-index.md#s25)

## Player staging 的职责

现有 jntracy-stage-player-code 工具按 Plan → Deploy → Validate 建立完整独立 Player 副本，并保留原资源、boot.config、StreamingAssets、签名插件和不属于本次替换的文件。检查 JNTracyClient 只有一份、IL2CPP 代码/metadata/符号配套。[S29](../08-references/source-index.md#s29)

| 状态 | 证明的事情 |
|---|---|
| Build 成功 | 指定目标由指定源码完成构建 |
| 静态部署 Validate | 文件集合、身份与约束满足 |
| 场景 ready | 某次运行达到可采集状态 |
| Capture/Query 通过 | 该次数据及指定域符合合同 |
| 性能/稳定性通过 | 需独立、有环境控制的验收 |

DXR 记录说明静态 31 项部署通过与历史采集成功仍可能伴随后续 Player device removed。不能将 Editor 成功等同 Player 稳定。[S04](../08-references/source-index.md#s04)

## 一次交付必须说明

- 使用哪个仓库、分支、commit，是否包含未提交修改。
- 哪套工具与 DLL/PDB，实际文件 hash。
- 原资源包和独立目标路径，替换集合。
- 图形 API、gfx-jobs、画质、分辨率、Editor/Player 差异。
- 实际完成哪些验收，失败/延期/人工待验有哪些。
- 保留什么证据，下一步如何恢复或复现。

本次文档生成没有执行构建、复制 Player、修改配置、启动游戏或重录。操作命令见 [运行手册](../05-operations/runbook.md)；每次运行前重新核对任务授权和工具合同。
