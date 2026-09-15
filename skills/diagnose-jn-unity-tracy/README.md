# JN Unity Tracy 深度性能分析技能

> 当前发布版 2.3.0：新增[原生配图与交互回查规范](references/native-profiler-views.md)，配套通用工具与自动展示门禁尚需实现；缺少能力时明确交付展示缺口，不冒充可交互。继续沿用 2.2.1：报告按主要样本帧号升序，“如何发现”仅留机器记录，不再逐条展开于正文。机器工作流合同仍兼容 2.2.0。按 [优先级与测试人员决策](references/priority-and-tester-review.md) 执行原有分流；本次不改变 Query、预算或深查范围。


版本2.1.0用于一个现有录制的Query原生扫描与源码深查。候选细分保留原始优先级和统计事实；报告按整理后的原因条目组织，展示具体帧、真实调用/等待链、耗时占比、源码解释与反例。

## 使用

- “用 diagnose-jn-unity-tracy 分析这个录制，按具体帧和源码解释性能原因。”
- “比较正常、高峰和持续偏慢帧，把同因实例归并并说明发现过程。”
- “继续上次未完成的源码深查，保留已有证据和必查队列。”

请先读 [SKILL.md](SKILL.md)，并遵循其中必读参考。源码未读不能完成深查；旧报告需要实际补齐performance_review证据，不支持自动补几段文字升级完成状态。

## 安装

从权威源码目录执行 `scripts/install-skill.ps1 -Force`。安装器保留本机local-profile.json和分析YAML。Query仍要求schema 1.35.0；本次技能升级不改变Query候选算法或预算阈值。

## 结构与验证

- [深查与报告合同](references/deep-analysis-and-report.md)：字段、来源、样本口径、原因归并与完成条件。
- [结构Schema](schemas/performance-review.schema.json)：机器记录格式。
- `scripts/classify-candidates.py`：由Query事实生成候选多维视图。
- `scripts/validate_analysis_result.py`：默认执行当前深查合同。
- `scripts/build_report.py`：默认生成原因正文及可读证据索引。
- `scripts/lint-skill.py`：无需外部依赖的语法、引用与合同检查。

历史测试使用显式 `--legacy-regression`，该模式不表示当前流程完成。正常报告必须经过原始收据、源码、发现过程、归并和全文可读性校验。格式检查通过不替代AI对因果解释及反证的复核。
