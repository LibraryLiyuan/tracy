# 原生视图 网页回查与报告分享

当前有两条展示路线。普通 .tracy 保留原生桌面及历史 Wasm 查看器；Stream 的新网页使用隐藏原生 Profiler 进程，把 framebuffer 送到网页 canvas，并把鼠标输入送回该 ImGui context。后者不是 Wasm，也不是只展示静态图片。[stream-native-views.md:1](../skills/diagnose-jn-unity-tracyV2/references/stream-native-views.md#L1)

## 工具分工

| 工具 | 职责 |
|---|---|
| prepare.py | 从保存的原生/Query 回执和 spec 建立强身份视图注册表 |
| start.py / server.py | 启动或复用本地服务和同一隐藏 renderer |
| viewer.html | 显示实时画面、输入、状态和显示比例 |
| export.py | 按视图顺序定位并导出原生 PNG 与回执 |
| StreamingView/Timeline | store 读取、原生定位、时间线、鼠标交互和 framebuffer |

实际 Python 源码在 V2/tools/stream-report-viewer，原生代码在 profiler/src/profiler。[prepare.py:1](../skills/diagnose-jn-unity-tracyV2/tools/stream-report-viewer/prepare.py#L1) [server.py:1](../skills/diagnose-jn-unity-tracyV2/tools/stream-report-viewer/server.py#L1) [TracyStreamingView.cpp:1](../profiler/src/profiler/TracyStreamingView.cpp#L1) [TracyStreamingTimeline.cpp:1](../profiler/src/profiler/TracyStreamingTimeline.cpp#L1)

## 视图身份

视图至少绑定当前 capture/source/revision、EXE、FrameSet 与帧索引、owner、事件 ref、起止区间和回执 SHA。Query CPU/GPU zone ref 尾数是 snapshot-order index，不是 EventStore ID、指针或 GUI 帧号。映射后要核对名称、owner 和端点，GUI 显示号单独记录，不能统一写 index+1。

没有逐事件证据时可以提供 frame-only；源码或配置假设可以链接“相关现场”。二者都不能声称已精确选择某个根因资源。[stream-native-views.md:1](../skills/diagnose-jn-unity-tracyV2/references/stream-native-views.md#L1)

## 准备与启动

spec 示例只说明结构，帧号和回执必须来自本轮：

```json
{"views":[{"id":"render-wait","title":"渲染线程等待","frame_index":3030,"event_receipt":"evidence/request-24.json","note":"完成端关系仍需核实"}]}
```

```powershell
python -X utf8 scripts/stream_views.py prepare --analysis C:\YourAnalysis\run-001 --spec C:\YourAnalysis\view-spec.json --output C:\YourAnalysis\views\viewer-config.json
python -X utf8 scripts/stream_views.py start --config C:\YourAnalysis\views\viewer-config.json --state-dir C:\YourAnalysis\views\runtime
python -X utf8 scripts/stream_views.py export --state-dir C:\YourAnalysis\views\runtime
```

wrapper 的机器设置参数是 `--local-config`；start 的 `--config` 是本轮注册表。服务 URL 从 runtime/server-state.json 读取，不猜端口。重启沿用已记录端口，端口冲突应明确失败，不悄悄改掉报告链接。[stream_views.py:1](../skills/diagnose-jn-unity-tracyV2/scripts/stream_views.py#L1) [start.py:1](../skills/diagnose-jn-unity-tracyV2/tools/stream-report-viewer/start.py#L1)

一个注册表复用一个 renderer，标签共享其当前视图。服务仅监听回环地址，按注册 view ID 工作。可缩放、平移、轨道展开/滚动、事件选择；当前不转发键盘/文本输入。[README.md:1](../skills/diagnose-jn-unity-tracyV2/tools/stream-report-viewer/README.md#L1)

## 三类验收

| 层次 | 必须证明 |
|---|---|
| 定位 | capture、帧、owner、事件、端点与回执一致；跨帧事件不硬塞进不重叠帧 |
| 图片 | 当前目标可见，导出来自原生 framebuffer；保存 PNG SHA，人工检查可读性 |
| 交互 | 浏览器实际输入引起原生状态与画面变化；不以 HTTP 200 或原生 ready 代替 |

export-validation 与 analysis audit 独立。失败视图不能复用上一张图；报告条目和 P0/P1 表格逐行绑定入口，不能只在总览附一个图库。[export.py:1](../skills/diagnose-jn-unity-tracyV2/tools/stream-report-viewer/export.py#L1)

## 响应性与显示清晰度

2.6.0 的“操作最终生效”曾掩盖约一秒延迟。当前实时通道在移动中投递输入，保存最新画面，按下/松开保持顺序，允许合并可替代的移动/滚轮。默认工程验收目标包含输入到服务器完成帧的 P95 小于 150 ms、2 秒拖动至少 25 张中间帧及 10 次真实范围变化、队列不超过 4；浏览器解码/绘制另列。这是工具验收目标，不是所有机器的承诺。

另一个历史问题是大窗口 RGBA 超过 8 MiB 通道后静默丢帧；当前固定离屏 framebuffer 与桌面窗口/DPI 独立。还要验证重新打开、多标签切换、导航对应的画面真正到达 canvas，不能无限显示定位中。

2.6.3 区分 1:1、2×整数放大和适应宽度。清晰 1:1 要满足 CSS 宽 × DPR ÷ 原图宽 = 1；时间轴缩放与显示缩放是两个操作。应检查不同 DPR、窄侧栏、大窗口、坐标映射和当前视图保持。[stream-native-views.md:1](../skills/diagnose-jn-unity-tracyV2/references/stream-native-views.md#L1)

## 报告内如何放图和链接

每项就地提供图片、文字链接、FrameSet、Query 索引/GUI 显示号、线程/事件和证据边界。图片链接与文字链接指向同一真实 view。普通 Markdown 的预览图片不等于能在所有阅读器里直接运行交互界面。

生成器状态要分开写：当前 Stream 有独立阅读版组装能力，旧 build_report.py 并未因此自动实现所有分册/配图/展示验收。

## 三种分享方式

| 交付目的 | 必须附带 |
|---|---|
| 离线阅读 | 主报告、分册、图片、源码快照、引用证据、相对链接与使用说明 |
| 原生交互回查 | 上述内容加录制/store、注册表、匹配 renderer/工具、启动器和本机路径处理 |
| 完整重分析 | 再增加可复现的 Skill、策略、原生分析器、版本/哈希与必要源码 |

localhost 指向接收者自己的机器。10 月 13 日约 463 MB 分享包支持离线阅读，但完整录制未复制；原分析目录的 Start-Viewer 依赖原录制和本机工具。不要把“离线报告可读”写成“同事无需其他材料即可完整交互或重分析”。[README.md:1](C:/Users/Admin/Documents/JN-Unity-T3/Analysis/T3-115721-Full-SKILL-20261013/README.md:1)
