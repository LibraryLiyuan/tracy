# 固定报告合同

## 1. 权威数据流

```text
MCP Evidence
→ evidence-manifest.json
→ AI analysis-result.json
→ validate_analysis_result.py
→ build_report.py
→ Markdown + SVG + MCP PNG
```

AI 不手写最终布局。Markdown 是权威结论，SVG 与 MCP PNG 是默认证据附件。HTML 默认关闭，只能由用户明确要求后以试验模式生成。

## 2. Analysis Result

Schema 位于 `schemas/analysis-result.schema.json`。每个显著 Signature 必须包含稳定键、显著性原因、影响范围、频率、Budget Debt、Primary Limiter、代表实例、因果链、假设账本、深度、Evidence、provenance、支持/不支持结论、优化方向、功能风险、理论收益上限、复测指标和置信度。

每个 Signature 都必须有独立 Evidence Card。`deepest_level != L6` 时必须携带最小 EvidenceGap，否则校验失败。

## 3. Evidence Manifest

Schema 位于 `schemas/evidence-manifest.schema.json`。每条 Evidence 记录 Trace identity、MCP method/params、request/response path 与 SHA-256、frame/time/thread/queue/entity refs、partial/truncated/cursor、attempt、quality 和 data path。

跨 Trace 引用、SHA 不符、数据路径逃逸、时间反转、Zone 越界、父子错误或未披露截断必须失败。Renderer 只从 Evidence 时间和 refs 计算坐标与关系。

## 4. 输出目录

```text
AnalysisReport/
├─ Performance-Analysis-Report.md
├─ Analysis-Process-and-Query-Audit.md
├─ Evidence-Index.md
├─ analysis-result.json
├─ evidence-manifest.json
├─ manifest.json
├─ report-validation.json
├─ evidence-data/
└─ visuals/
   └─ assets/
      ├─ data/
      ├─ frame-images/
      ├─ static-previews/
      └─ exports/
```

当且仅当用户明确要求并传入 `--html` 时，额外生成 `visuals/index.html`；`--html --standalone` 再生成 `standalone.html`。主 HTML 通过本地 `<script src>` 加载安全数据分片，不使用 `fetch`、CDN 或网络。

## 5. Markdown 固定结构

1. Trace 身份与证据质量。
2. 性能结论总览。
3. Frame Class 分布。
4. Bottleneck Signature 地图。
5. 关键路径与等待传播。
6. 独立 Evidence Cards。
7. CPU 内存与 GPU 显存专项。
8. Tracy Runtime 采集与 Tracy Analysis 离线压力。
9. Evidence Gaps。
10. 优化优先级和复测方案。
11. 全域健康统计附录。
12. 查询审计索引。

无异常域只出现在健康附录，不为满足模板强造正文。

每张卡必须让读者看到：影响/频率/Debt、典型/最严重/重复实例、Primary Limiter、完整因果链、支持和反证、调用栈/Source/Pass/资源链、根因或最小缺口、优化方向、风险、理论上限、复测指标、置信度和状态。

## 6. 固定视觉证据

普通卡在对应证据存在时生成：

1. 帧上下文多轨 Timeline。
2. Signature 聚焦 Zone Timeline。
3. 因果关系图。
4. FrameImage（若存在）。
5. Stack/Source。

内存/显存卡：

1. 全 Trace 趋势与预算线。
2. 峰值/异常窗口。
3. 类型和增量。
4. Pass→Resource→Allocation/Heap。
5. 对应 FrameImage（若存在）。

静态 SVG 与 MCP PNG 直接嵌入 Markdown。Timeline 没有包含可验证 Zone 坐标的 Evidence 时不生成 Timeline SVG；没有已证明 typed relation 时不生成因果 SVG；资源关系 Evidence 没有可验证节点时不生成资源图。Markdown 必须用文字披露缺失原因，禁止生成只有 `EvidenceUnavailable`/`EvidenceGap` 字样的空图。只有显式启用 HTML 时，才附加 `visuals/index.html#signature-<id>` 或 `#frame-<id>` 链接。

静态布局必须满足：

- 同一 Lane 的嵌套或时间重叠 Zone 分配到互不覆盖的行，Lane 高度按实际行数动态计算。
- Zone、Lane 和标题文字不得越过其可用区域；显示文本可省略，但 `<title>` 保留完整值。
- 因果节点使用动态高度和多行文本；关系箭头不得穿过节点文字。
- 单时间点内存证据渲染为带名称和值的快照比较图，不伪装成趋势折线。
- 无数据不生成 SVG 文件；旧输出目录中的同名陈旧 SVG 必须在重建时清理。

FrameImage 必须来自 MCP `resources/read` 返回的 `image/png`，保存 resource URI、MIME 和 PNG SHA-256。报告生成器拒绝 Skill 内本地解码或重建的图片。

## 7. Timeline LOD

```text
Complete
VisualAggregationOnly
QueryPartial
QueryTruncated
EvidenceUnavailable
```

`EvidenceUnavailable` 是报告质量状态，不是要求生成空 SVG。小于像素的 Zone 初始可显示密度带，但放大后必须恢复真实 Zone；LOD 只影响显示，不影响统计或结论。Query partial/truncated 必须显示质量警告。

## 8. 单 Trace 开销措辞

主报告固定包含：

```text
TotalCaptureOverhead = NotMeasuredSingleTrace
```

可以报告 Trace 内观测到的 producer 自耗时、Sampling、Context Switch、FrameImage、GPU Reference、Catalog、写盘和积压，但不能写成 Tracy 总体开销。

## 9. 确定性与安全

- `--deterministic` 使用 Analysis Result 内固定生成时间和稳定排序。
- 同一输入、配置、生成器和模板版本的权威产物 SHA-256 一致。
- 所有 Trace 字符串做 Markdown 转义；启用 HTML 时同时做 HTML/JavaScript 转义。
- 文件名只使用生成的稳定 ID，不使用 Marker/Resource 名称。
- 可选 HTML 不读取 `.tracy`，不要求 Query 常驻。
- `manifest.json` 记录版本、配置 SHA 和所有产物身份。

## 10. 完成门禁

- 所有显著 Signature 有 Card，达到 L6 或有最小 EvidenceGap。
- Wait 归因有真实 relation 或 `UnresolvedWaitChain`。
- GPU 推断与内存口径符合证据边界。
- 默认产物不包含 HTML，所有 Markdown 链接可解析。
- 静态图不存在文字/Zone/节点重叠；无坐标或无关系的卡片不产生占位 SVG。
- 显式启用 HTML 时，Markdown/HTML 数字、结论和 Evidence ID 一致，所有锚点可解析且可离线双击打开。
- 两次确定性构建 SHA 一致。
- `report-validation.json.passed=true`。
