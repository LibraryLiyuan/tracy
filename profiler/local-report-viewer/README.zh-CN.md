# Tracy 本地报告查看器（工具预览 0.1.0）

本工具将本地 Markdown 的视图链接接到真正的 Wasm64 Profiler，并导出同一定位的原生 Canvas。它不分析 Trace、不修改 Query 统计，不自动更新既有报告。工具仍待用户验收，不是全量报告集成的完成声明。

## 使用

准备 viewer-config.json，登记 Trace 路径及 SHA256，再登记视图的 FrameSet、Query 帧索引和原始起止时间。普通纳秒数、线程ID使用十进制字符串。可选 threads 必须同时给出原生线程ID与名称；可选 event 必须给出线程ID、精确起止时间和名称。标识来自已核验的 Query 证据，不能把 Query ref 字符串直接当 C++ 指针。

运行 Start-Viewer.ps1，传入 Config、WebRoot、Python、StateFile；可用 ReportDir 指定本工具生成的 Markdown 目录，服务端口变化后只刷新带工具标记的阅读页链接。默认只监听127.0.0.1；默认端口占用时自动选择空闲端口。相同配置和构建的已有服务会复用。

点击 /open/<view-id> 进入 Profiler。没有既有实例时在当前标签加载，不依赖弹窗。已有同Trace实例时通过 BroadcastChannel 请求导航；浏览器可能不允许自动把旧标签置前，此时入口页会说明已定位，并提供在当前标签打开的后备链接。只有定位完成才显示成功。手动打开 localhost 与127.0.0.1属于不同浏览器来源，不共享实例。

滚轮缩放；右键拖动时间和轨道；左键点击原生事件查看详情。“回到报告位置”恢复登记视图。“事件详情”只选择经时间、线程和名称核对的CPU Zone。Query帧索引与原生显示号分开显示，例如命名FrameSet通常显示索引加一；以原生返回值为准。其他轨道不删除，不改变后台采样处理。

通过原生菜单另开录制会失去报告身份绑定，应重新从登记入口打开；不允许沿用旧Trace的回执。配置或Web构建更新后重新运行启动脚本，并关闭旧的Profiler标签。

## 导图

导图机需要 Node.js、Playwright 和 Chrome；阅读者只需要本地服务和支持WebAssembly Memory64的浏览器。开发机实测为 Playwright 1.62.1、Chrome 153。

```text
node export_views.cjs --server http://127.0.0.1:8140 --views <登记的视图ID> --output <独立输出目录>
```

可传入逗号分隔的多个视图。相同Trace只加载一次；一个批次中不同Trace会明确拒绝，应另开实例/批次。输出原生帧PNG、可选事件详情PNG、Markdown和身份/定位回执。导图等待真实后台完成与目标线程布局就绪，失败则写失败回执，不把残帧当成功。导出后仍须实际检查图片的文字、目标可见性、遮挡和所需关联轨道。

## 开发构建

build_viewer.py 接受 Emscripten SDK、输出目录、CMake、Ninja、本机 embed 工具，以及可选依赖缓存。首次依赖准备与编译属于工具开发，不在每次分析中重复。必须使用 MEMORY64=1；上限16 GiB按需增长，不代表可打开16GB的压缩Trace，也不是录制时长承诺。

若使用本地已打补丁的 ImGui/PPQSort，必须同时传入两个 source 参数；否则走正常依赖下载和补丁流程。成功输出 viewer-build.json，记录实际JS/Wasm身份。构建不嵌入、不复制任何Trace。

## 接口与验证

- /api/capabilities：协议、注册配置和构建身份；Job对象/GPU资源精确选择不宣称支持。
- /api/views/<id>：定位记录及登记录制的下载地址；不暴露本机文件路径。
- /api/captures/<id>/trace：仅提供已登记且文件状态未变的Trace，浏览器再核对SHA。
- 原生 tracyReportNavigate：核验捕获身份、FrameSet、帧索引/范围、可选线程和CPU事件后应用导航；错误不改变当前视图。
- 原生 tracyReportState：返回实际范围、原生显示号、后台处理、布局与选择状态。

单元测试：`python -m unittest discover -s tests`。真实浏览器测试为 tests/test_web.cjs 和 tests/test_handoff.cjs，需要先启动验收配置指定的本地服务，输出位置由 TRACY_TEST_OUTPUT 指定。真实Trace不入Git；本次验收配置和随机抽样结果保存在工作区独立目录。

本次已发现原生 Running state time 可出现明显异常，且GUI提示上下文切换数据不完整。它不是本工具的分析结论，不能用于CPU运行时间或优化收益判断。导航/原生渲染测试通过不表示所有原生统计字段正确，也不代表全部Profiler功能已回归。
