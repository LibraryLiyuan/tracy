# GPU Resource Catalog 分析合同

## 1. 能力边界

Catalog 建立 GPU/Gfx 证据链：

```text
GPU Pass
→ Resource / View / Range / Subresource
→ Logical Binding
→ GpuResource
→ Physical Allocation / Heap Range
→ Residency / DXGI
```

它不自动提供 UnityObject、Asset GUID、Prefab、Scene、Bundle 或磁盘路径。没有明确 relation 时返回 `unavailable`，不得用名称、大小或时间接近度猜测高层归属。

开始资源级分析前查询 capability、Catalog status 和 validation。协议、section、Query/Catalog schema 的期望值从 `project-profile.trace_contract` 读取，只作为元数据，不写入 Signature 或诊断标题。

## 2. 质量门禁

检查：

- Connection generation 状态和 Bootstrap 完成度。
- Batch sequence、record count、checksum 和 GenerationEnd。
- Core dropped/gap/overflow。
- pointer lifetime unresolved、ambiguous 和 open boundary。
- RangeSet/ResourceSet partial/truncated。
- reconnect 后资源快照和增量是否连续。

结果：

- `complete`：允许资源级双向结论。
- `building`：只分析已明确完成的时间范围。
- `partial_enrichment`：Core 可用，名称/View/Range 缺口必须逐项披露。
- `invalid_core_gap`：禁止 Pass→Resource 和资源来源结论；DXGI/physical 独立域仍可用时继续分析。

Catalog invalid 不能解释为“没有资源”。

## 3. 身份和生命周期

`GpuResourceId` 表示一个真实 GPU Resource 生命周期。pointer 只用于 Worker 在事件时间解析，报告不暴露裸指针。pointer reuse 必须由 lifetime interval 区分。

连接前资源：

```text
create_time = open_boundary
create_stack = unavailable_pre_capture
```

imported/borrowed/swapchain 资源可使用 first-observed identity，但不能伪造创建时间或所有权。

## 4. 分类与名称

正交维度至少包含：

```text
ResourceClass
Dimension
PrimaryKind
DeclaredUsageMask
ObservedUsageMask
MemoryDomain
BackendFlags
ClassificationProvenance
```

UnknownResource 必须保留并计入物理显存。名称必须显示 provenance；GeneratedDescription 不是 Unity Asset 名称。截断名称保留原始长度和哈希，报告不展开绝对路径。

## 5. 内存口径

分别查询和报告：

```text
resource_capacity_bytes
physical_allocation_bytes
resident_bytes
owned_bytes
referenced_working_set
direct_range_bytes
inclusive_range_bytes
logical_subresource_bytes
untracked_bytes
```

规则：

- committed/placed/suballocation/heap 物理范围只计一次。
- alias/shared range 的参与资源不能重复相加为物理总量。
- PrimaryOwner 只用于无重复 owner rollup，不代表唯一使用者。
- Pass working set 在该节点内去重；兄弟 Pass 可引用同一资源，不能相加成总物理显存。
- DXGI usage/budget 是采样值；engine-known physical 是事件级核算。
- `untracked` 只是对账差额，不自动等于泄漏。

## 6. View、Range 与 Subresource

可证明路径返回：

```text
Pass → Resource → ViewDefinition → BufferRange / TextureSubresource → Usage
```

BufferRange 来源可包括 VBV、IBV、CBV、SRV/UAV、Copy、Indirect、Shader Table、RTAS input、Dynamic VBO 和 VG range。TextureSubresource 可包括 RTV/DSV/SRV/UAV、Copy/Resolve、RenderGraph handle、BackBuffer 和项目渲染目标。

Root descriptor、bindless 动态索引等无法证明长度或实际 View 时必须返回 Unknown，不能用 whole resource 冒充精确 Range。

`direct_range_bytes` 是 Pass 直接证据；`inclusive_range_bytes` 是逻辑父节点的确定性并集。报告必须标明 direct/inclusive。

## 7. Mesh、Dynamic VBO、RenderGraph

普通 Mesh 可证明的 GPU 信息包括 Vertex/Index Buffer、stream、offset/length、stride、vertex/index count、index format、deformed buffer 和 RTAS contributor。没有 UnityObject/Asset relation 时只能称为 Mesh buffer，不声明具体 Asset。

Dynamic VBO 使用长期 Buffer + 每帧 RangeSet；每个 draw range 不创建永久高基数实体。同 Pass 内重叠或相邻范围可合并，但要保留 coverage/exactness。

RenderGraph 使用 LogicalDefinition/Binding。稳定 graph key 和描述映射到 physical resource；alias range 的物理 bytes 只计一次。同名资源不能仅凭名称合并。

## 8. Virtual Geometry 与 RTAS

VG 持续证据可包括：Buffer 类型/容量、heap 占用、RuntimeResource、Page Definition、Residency lifetime 和 Pass→VG Buffer/Range。Page slot 复用必须产生新的 residency lifetime，不能串历史。

Periodic/Manual checkpoint 才能证明的 Page/Cluster/Descriptor 精确集合必须标为 `PeriodicExact` 或 `ManualExact`；非采样帧返回 `NotSampled`，不是空集合。

RTAS 链：

```text
Vertex/Index/AABB → BLAS
BLAS set → TLAS
Scratch → Build/Update
ShaderTable → DispatchRays
```

per-instance BLAS/transform 等只在对应详细证据存在时报告。

## 9. Query 路由

优先：

```text
gpu.catalog.status
gpu.catalog.validation
gpu.memory.peak / by_type / by_pass / churn
gpu.resource.search / get / explain
gpu.resource.lifetime / allocations / references / views
gpu.resource.mesh_buffers / raytracing_chain / vg_pages
gpu.pass.resources / gpu.pass.vg_evidence
```

方法名以 MCP capability 为准。`gpu.resource.explain` 是单资源主入口；大型结果分页，不以截断第一页概括全体。

每个资源结论至少包含 Resource ID、类型/名称/provenance、capacity/physical/resident、allocation/heap/alias、lifetime、callsite provenance、Pass/Queue/frame、Range/Subresource、quality/exactness 和 unavailable reason。
