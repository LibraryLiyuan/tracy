# 从慢函数追到原因：补齐容易遗漏的分析层（2.5.0）

## 完成口径

“所有严重帧有窗口”“找到了长marker”只完成覆盖/位置定位。报告分别写 `scope_located`、`mechanism_supported`、`runtime_cause_verified`、`benefit_measured`；不要把定位到函数当作根因已查清，更不能把源码建议的收益相加承诺稳定60FPS。未闭环时写下一条能区分假设的最小证据。

## 针对不同线索，往哪里再追一步

| 已发现的入口 | 还必须核对的范围 | 可以结束在什么边界 |
|---|---|---|
| Main/Render等Job | 同一窗口所有Worker；帮助执行、serialized lane、allocator/driver调用及真实producer/completion身份 | 未找到完成端可以unresolved，但先查Worker，不能因报告只展示Main/Render而跳过 |
| 通用CreateResource/Upload/Release/Apply | 完整上游caller、资源用途/类型/名称与lifetime，调用条件、已有复用和dirty gate；创建后是否马上失活/销毁 | 不能只建议“池化/异步”；至少说明具体哪条调用链和哪个已有条件失效或仍未知 |
| Shader/PSO实时创建 | 采集→解析器→sub-assets枚举→预热资产生成→当前质量档路径→加载→warmup实际执行→cache key命中 | 区分shader编译和PSO driver创建；源码缺口与运行时漏预热分别证明，不能用创建次数冒充唯一变体数 |
| 锁/看似Async的API | 锁获取点、可能持锁的同资源上下文、锁内工作和Release时刻；完整父路径是否共享context | 时间重叠只支持假设；“所有N个案例已确认同锁”须逐例持锁/等待证据，不能只比较结束时刻 |
| 容量不高却有residency提交停顿 | resident/evicted状态曲线、使用量/预算、实际生效策略、aging/pressure/driver路径 | 低VRAM不能排除驱逐；evicted bytes是状态量，不能直接称累计搬运量。调高阈值能否消除stall需同条件复测 |
| 脚本UI/加载入口 | 具体组件Awake/OnEnable、同步LoadAsset、prefab默认依赖、池初始状态和分帧机制 | 业务函数已找到仍要查落到哪个组件/资源；不要只停在引擎Awake或LoadAsset容器 |
| 多管理器/队列预算 | 预算是实例/层级/相机还是全局；实际调用次数和实例身份，单个操作能否跨预算 | 12次Update不等于12个管理器；单实例2ms不等于全帧2ms，源码结构与实例数量分别说明 |
| 稳态余量不足 | 低单次高频成本：PluginEvent.Flush、参数设置、整buffer上传、重复小分配及回收 | 不能只挑self最大几项或>33ms帧；保留native count/total和正常对照窗口，不能用邻帧混合窗口推算全程均值 |

先运行 `python -X utf8 scripts/stream_review.py --run NATIVE_RUN --output NEW_REVIEW_DIRECTORY`。它检索**全部保存的CPU窗口和全部线程**，保留Worker长作用域、资源创建上游、等待、PSO和短PluginEvent.Flush的原生字段/证据指针。它不解析trace、不重新统计性能、不自动确认根因；生成的是调查义务索引，不是新增性能候选或根因数。随后按原生严重性、用户关注和反证选择定向查询；不要机械执行每个索引项，也不以Top2/TopN代替必查范围。

## 收益和配置不能跳过的检查

- 所有“每帧N次”“N个变体”“N个管理器”“N个案例都成立”要有匹配语义的原生统计/实例身份。不得将调用数当唯一对象数，将窗口内次数当全程频率。
- 读实际boot、TimeManager、画质、插件event ID/flags和warmup路径；配置文件当前值与capture生效值分开。默认值、注释与某个Editor Preview event不能替代运行中的对应事件。
- `maximumDeltaTime/fixedDeltaTime`只是配置比值，不能证明本录制实际发生了该次数追帧，也不能无条件推导“最多2步”。
- partial结果即使count看起来齐全仍不可用于结论。先保存失败，再缩范围、分页或在允许范围提高受支持预算；不能修改回执的partial标志。
- 与外部报告比较时先对齐capture身份、FrameSet、区间、30ms/33.333ms等阈值及full/clipped口径；无法复现的数值保留为未核实，不照抄也不直接指责错误。
- 源码存在无dirty上传、同步加载或单sub-asset枚举，可以作为已确认源码事实；把它绑定到某个具体卡顿、资源或收益还需要独立证据。明确这两个层级。

## 防止本轮遗漏再发生的交付检查

对每个主要问题，在调查账本回答：具体线程/函数是什么？上游业务/资源/配置是什么？Worker/持锁侧是否查过？已有缓存/预算为什么没挡住？源码事实与运行条件分别证明到哪一步？用什么正常/反例样本检验？尚未验证的收益是多少（通常写未测）？

没有答案就保留 `scope_located` 和缺口。文档校验或窗口覆盖passed只能证明相应范围，不应替代这份人工因果复核。
