# Stream：逐问题原生截图与回查链接（2.6.3）

本分支已实现 `stream-report-1` 网页内嵌原生 Profiler：本地服务启动隐藏的原生 renderer，直接读取 stream，网页 canvas 显示原生 framebuffer，鼠标事件只投递到该 ImGui context。可在网页缩放、平移、展开线程、选择事件，另有图库和 Markdown 离线预览。不是只有截图链接，也不要求切到桌面窗口；不能把此机制称为 Wasm。普通 `.tracy` 继续使用原展示路线。

## 工具与输入

`config/stream-viewer-local.json` 指向已验证工具目录、Profiler EXE 与 SHA-256。工具目录包含 prepare.py、server.py、viewer.html、start.py、export.py；缺失时报告具体能力缺口，普通分析不自行构建。用户明确要求修工具/技能时可在该任务中实施并验证。工具源码位于 Tracy 仓库 `profiler/stream-report-viewer`，原生导航位于 StreamingView/Timeline。

输入是已完成的原生分析目录（native-run/manifest.json、frames.ndjson、owners.json、triage.json）、完整同捕获的事件 tree 回执，以及视图 spec。每个条目需覆盖到问题与样本；没有逐事件证据时允许登记 frame-only，但不能声称选择了确切函数。

```json
{"views":[{"id":"render-wait","title":"渲染线程等待 SBT","frame_index":3030,"event_receipt":"severe-exact/request-24.json","note":"等待本身不是生产端根因证明"}]}
```

示例帧/路径仅解释结构，必须替换成当前捕获的真实回执。frame-only 使用 focus_owner_ref；不猜线程 ref。等待方与有证据的工作方、跨线程资源问题可登记多个视图；没有 relation 时保持未知。源码/config-only 条目使用“相关现场”链接，并说明尚未绑定具体资源，不能伪装成确切根因样本。

## 执行

```powershell
python -X utf8 scripts/stream_views.py prepare --analysis <分析目录> --spec <view-spec.json> --output <新目录/viewer-config.json>
python -X utf8 scripts/stream_views.py start --config <viewer-config.json> --state-dir <新目录/runtime>
python -X utf8 scripts/stream_views.py export --state-dir <新目录/runtime>
```

注意：wrapper 自身的机器配置选项是 `--local-config`；传给 start 的 `--config` 是本次视图注册表。服务首次分配地址并保存在 runtime/server-state.json，重启沿用该端口；冲突时失败，不悄悄产生失效的旧链接。报告附 start 命令/脚本，不承诺服务退出后旧 localhost 仍可访问。只使用真实返回的 URL。

prepare 只读取已保存 Query JSON，不解析 stream 或重算统计。它保存事件回执 SHA、强捕获身份与 EXE 身份。原生 Profiler 直接读 stream 的发布 EventStore；不转换完整 `.tracy`。同一注册表复用一个隐藏的原生进程。网页连接依赖本地服务；断网可用，服务退出后要重启。

关键编号合同：Segment Query CPU/GPU zone ref 尾数是 snapshot-order index，**不是 EventStore ID、指针或 GUI 帧号**。原生程序按 thread/group 顺序和 cursor 映射，再核对事件名称、owner、起止时间。GPU context ref 是 context 顺序索引。帧索引独立与原生 frame start/end 核对，保存 GUI 显示号；不得固定 `index+1`。跨帧事件记录完整范围和锚定帧，不将邻接窗口的事件硬塞进不重叠帧。

## 交付与验收

export 顺序定位全部登记视图，等待完整源验证、后台加载、目标轨道及事件可见，再由原生 OpenGL framebuffer 导出 PNG。保存每视图实际解析回执、PNG SHA 与 export-validation.json；失败就报错，不能用上一视图图片顶替。复用截图前核对身份、位置和二进制版本。

每条观察正文就地加入：问题相关原生图片、同一 view 的 `/open/<id>` 链接、FrameSet、Query 索引/GUI 显示号、事件/线程和范围、证据边界。P0/P1 表格每行也有入口；不能只给总览图库。保留冻结原报告，在新目录组装阅读版，核对原正文和所有分析证据链接仍有效。

仍执行 native-profiler-views.md 的三类验收：数据定位、实际图片可读性、会话交互。程序导航与网页鼠标交互分别记录。必须通过实际网页输入验证缩放、平移、轨道展开/滚动和事件选择，并核对原生状态变化，不能只因 HTTP 200 或图片变化就写 passed。检查失败需要注明具体范围，而不是笼统写“展示缺失”。

保存 view-manifest.json、presentation-validation.json、逐问题覆盖表和过程 MD。当前是独立阅读版组装，旧 build_report.py 不因此自动获得能力；分别登记 assembly complete 与 core generator pending。图片和数据定位完成可独立交付，但不要把任何未执行交互写为完成。

## 网页响应性验收（2.6.1）

2.6.0 的“单次操作最终生效”验收不足：真实反馈发现连续操作有约1秒延迟，不能继续据此称为可用。使用 stream-report-2 的实时输入/帧通道，保留严格定位和离线截图合同。不要退回逐动作落盘截图、串行等待后台完全ready，或松手后才回放整段拖动。

在真实录制、已定位的视图上运行工具目录的 `benchmark.py --url <服务URL> --view <已登记ID> --output <新验收目录/latency.json>`。同时验证浏览器的连续拖动/滚轮和事件选择，记录实际画布更新，而非只查HTTP状态。默认工程验收目标：单次输入到服务器完成帧的P95小于150ms，2秒拖动期间至少25张中间帧、至少10个真实范围变化、输入队列不超过4、松手确认小于150ms。浏览器解码/绘制延迟须另列；这些数值是工具验收指标，不是游戏性能统计，也不是所有机器的性能承诺。

输入事件序号确保按下/松开不丢失；只合并可替代的移动/滚轮。鼠标移出、窗口失焦和取消拖动要释放按钮。画面只保存最新帧，输入不能排在帧解码后面；后台读取时允许显示带读取状态的原生画面。共享内存帧需guard/尺寸检查，不降低原始截图和Query身份门禁。空闲必须停止高频重绘，不能用长期占满CPU掩盖延迟。

报告同时记录功能正确性与响应性结果，旧静态导出通过不能沿用为实时性能通过。用户反馈卡顿时先重现、测基线、修复后比较；保留失败数据与旧版本，不把简单可点击称为流畅。

## 打开成功与 Markdown 接入（2.6.2）

服务支持 `stream-report-2.1`，原生精确定位回执沿用 `stream-report-1`。必须核对 **本次 navigation 对应的实时画面已到达并实际画到浏览器 canvas**；单看原生 `phase=ready` 不够。用户反馈曾暴露2560×1369窗口的RGBA超出8MiB传输容量而静默丢帧：现在网页使用固定离屏framebuffer，显示尺寸独立于桌面窗口/DPI/最小化。窗口尺寸变化、重新打开和多标签切换是这类修复的必要回归；不要重置用户全局显示设置来掩盖问题。

无画面、源验证失败、查看器退出、别的标签切换到新导航，均应给出明确状态和重新定位入口，不能无限显示“定位中”。对同一问题的并发初始加载复用正在进行的导航。正常请求取消不应变成日志中的异常洪泛。

每条报告同时提供可读文字链接与可点击离线预览：

```markdown
[在 Tracy 中查看具体问题](http://127.0.0.1:实际端口/open/已登记view-id)
[![具体问题预览](visuals/assets/native-profiler/已登记view-id.png)](http://127.0.0.1:实际端口/open/已登记view-id)
```

以真实注册表替换示例；从Markdown预览/阅读模式点击。链接本身不启动服务，需提供经过验证的本机启动脚本。新增阅读版应包括简短使用说明，并把启动器连接到当前EXE/注册表；旧链接若继续交付，应验证其转发链实际可达。普通Markdown中的预览图片与网页交互是两个不同表面，不宣称所有Markdown阅读器都能直接嵌入运行时。

## 字体和像素显示（2.6.3）

无损PNG也会因浏览器二次缩放变糊。检查原图尺寸、canvas CSS尺寸及devicePixelRatio，实际屏幕缩放比为 CSS宽×DPR÷原图宽。默认清晰1:1应满足1，不能用width:100%把1600像素原图任意拉伸或压缩；2×整数放大保留像素感，适应宽度明确是可能产生插值的概览模式，不称为高清重绘。

当前网页提供这三种显示模式、滚动区域和全屏内可见的显示选择。时间轴放大与画面显示缩放分别标注。检查1/1.25/1.5/2倍DPR、窄侧栏与大窗口，鼠标坐标必须仍映射回原图。显示设置和页面刷新应尽量接续同一问题当前视图，不默认跳回报告锚点；“回到报告位置”保留显式复位。

显示层修复可直接更新网页，无须重编译分析器或重导所有原生图片。运行工具目录test_display.js核验像素比例/命中换算，实际页面验证模式切换及当前时间范围保留。若需要铺满高DPI屏幕并保留原生字形细节，需单独实现受预算限制的原生重绘；不得把CSS插值或锐化冒充该能力。
