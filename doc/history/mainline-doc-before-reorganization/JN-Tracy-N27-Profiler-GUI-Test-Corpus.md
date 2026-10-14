> 历史归档：mainline f49190f2541c 中的历史资料。当前版本与操作入口见 [文档首页](../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# N27 Profiler GUI 测试语料

## Protocol 90 / N27

- `C:/workflow/jnunity/build/JNTracyCaptures/N27-C8-Synthetic/N27-C8-Protocol90-Synthetic.tracy`
- `C:/workflow/jnunity/build/JNTracyCaptures/N27-C8-Synthetic/N27-C8-Diagnostic.tracy`
- `C:/workflow/jnunity/build/JNTracyCaptures/N27-C8-Final-HighEvidence-Reconnect/N27-C8-Final-HighEvidence-Reconnect.tracy`

## 负向与旧版本

- `C:/Users/Admin/Documents/JN-Unity-T3/PlayerCaptures/N27-Stage2205-Protocol88-Smoke-20260820-220500.tracy`
- synthetic invalid/corrupt traces 在 GUI 单元测试中生成，不覆盖现有 Trace。

## 大型 Trace

- 在 G8 从现有 Protocol 90 捕获中按文件大小选择最大可读取样本，并将 SHA-256 和大小写入验收报告。

所有语料只读使用；Profiler 缓存不得与 Trace 相邻。
