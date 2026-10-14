> 历史归档：mainline f49190f2541c 中的历史资料。当前版本与操作入口见 [文档首页](../../README.zh-CN.md)。原文中的“当前”、完成状态、版本与源码行号均指原记录时点；链接已按本次目录整理，未找到的旧目标保留为文字引用。

# 转换器内存归因检查点

用户授权先做归因，再讨论针对性处理。本提交提供显式开启的开发诊断能力，没有实施磁盘外置或改变记录保存语义。

## 工具

开发版Converter新增`--memory-attribution <jsonl>`，在数据锁保护下采集各域数量、逻辑字节和自有容量。可使用`--diagnostic-private-limit <bytes>`提前结束重放，不发布截断Trace。原系统保护条件不变；将物理内存负载名称改正，并另外记录GetPerformanceInfo系统提交额度。

Slab与借用其存储的Vector不重复计账。哈希payload估计和Slab内逻辑对象量只作解释项，不能加入已知容量总和。快照会遍历容器并持锁，属于诊断功能，不应用其运行时间作为性能初评。

## 已运行结果

指定30分钟Protocol90录制的独立副本，一次前缀诊断，在已发布36,608/226,710个client records、16 GiB诊断停止线附近退出5。最后快照Private Bytes为16.095627 GiB，Working Set为13.137096 GiB；已知容器容量15.311790 GiB，相当于Private Bytes的95.13%。未写出候选Trace。

| 持有者 | 数量 | 容量GiB |
|---|---:|---:|
| GPU Reference Use | 85,368,618 | 4.045667 |
| Job Stage | 65,640,635 | 3.213826 |
| Tracy Slab | 一个共享分配器 | 2.250242 |
| ResourceSet定义成员 | 71,325,531 | 0.797125 |
| Catalog全部相关表和字符串堆 | 多表合计 | 1.432301 |

Reference Use和Job Stage合计7.259493 GiB，其中约1.5972 GiB为数组容量余量。二者有效记录仍有5.662279 GiB，缩小reserve不能解决历史不断累积的问题。

最后快照系统CommitTotal/CommitLimit约98.84%，旧三个物理/进程相对保护条件flags仍为0；内部memUsage仅3.899243 GiB。必须区分进程私有提交、系统commit与物理内存。不能据此倒推上次27.3%失败的具体保护条件，也不能把其他进程的系统commit归给Converter。

本次只覆盖已重放前缀，未度量最终关联、Write、Validate或全量数量。单域一亿条加载上限仍是待处理边界，不能由前缀直接断言全量数量。

## 验证

- 归因原语红/绿回归，项目CTest对应1/1通过。
- Protocol90小型夹具基线/诊断版转换与重开通过；全部JN表逐字段和质量比较通过，Resource与Batch各1,001条。
- 内部序列化表示对照有差异，未用于宣称全数据等价；小型逐字段比较仅覆盖该夹具JN域。
- 一次大输入归因，共11快照，持锁累计约23.69秒；原始输入身份一致，没有完整性能结论。

建议顺序：真实资源保护 → Reference Use/Job Stage接收、顺序写出及有界重开 → ResourceSet定义字典 → Catalog与剩余域。JobStage虽然有sink枚举，当前文件加载调用尚未接入该sink，迁移时需要补齐。

工作区完整报告与证据：`GpuCatalog-DiskIndex/Attribution-20260915/Memory-Attribution-Report.zh-CN.md`、`summary.json`、`large/memory.jsonl`、`large/identity.json`及测试回执。诊断EXE的SHA256为`d1c4c2d76d9bee672337501294f63e889784fc851faaa025fbf2d1fbe680e34a`。
