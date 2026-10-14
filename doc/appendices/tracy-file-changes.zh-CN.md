# Tracy 变更范围与完整清单

比较本地 v0.13.1 的共同祖先 `05cceee0df3b8d7c6fa87e9638af311dbabc63cb` 与 mainline `f49190f2541c8da739d6c74fd2fe34eeff222e5e`，共 915 个文件。包含上游和定制变化、两份 Skill、第三方、测试与历史文档，不用于认领原创行数。

[完整 CSV](../evidence/tracy-changes.csv) · [完整 JSON](../evidence/tracy-changes.json) · [源码地图](../02-tracy-source-map.zh-CN.md)

| 顶层目录或文件 | 变更文件 | 新增 | 删除 |
|---|---:|---:|---:|
| .github | 1 | 53 | 0 |
| .gitignore | 1 | 2 | 0 |
| CMakeLists.txt | 1 | 1 | 0 |
| analysis | 66 | 27572 | 0 |
| capture | 54 | 6772 | 38 |
| capture-gui | 21 | 4086 | 0 |
| cmake | 8 | 248 | 6 |
| csvexport | 1 | 6 | 0 |
| doc | 28 | 3045 | 0 |
| jnclient | 4 | 1444 | 0 |
| profiler | 63 | 18012 | 186 |
| public | 16 | 3734 | 162 |
| query | 218 | 88584 | 0 |
| server | 23 | 7012 | 1826 |
| skills | 313 | 57233 | 0 |
| storage | 17 | 2413 | 0 |
| stream | 78 | 306757 | 0 |
| test | 1 | 62 | 0 |
| update | 1 | 6 | 1 |

mainline 与 dev 的差异另看 [分支关系](../evidence/branch-relations.json)。tag 至 mainline 是累计范围；当前 doc 的工作区删除不属于该提交树差异。

优先阅读 capture-gui/stream 的录制发布、query/analysis 的读取统计、profiler 的 Stream 回查、V2 Skill 编排，再按清单回溯历史。
