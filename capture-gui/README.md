# Tracy Capture GUI

Windows x64 独立录制工具。界面使用 C++20 / Dear ImGui / GLFW；录制、转换、Query 校验在独立进程中执行。无需安装 AI 软件、Python 或 PowerShell 即可运行。

## 使用

1. 将 `tracy-capture-gui.exe`、`tracy-capture.exe`、`tracy-stream-convert.exe`、`tracy-query.exe` 放在同一目录，或在设置中选择匹配的工具目录。Query 必须为 schema **1.35.0**。
2. 选择启动 Player 或连接本机已有 Player / Editor。默认勾选管理员启动；UAC 需要用户操作。
3. 在主区域选择实际存在的 INI。默认优先使用 `JNTracy.HighEvidence.ini`；可浏览自定义文件。INI 从目标程序目录、当前 INI 所在目录和工具旁 `presets` 目录列出。
4. 填写或沿用录制名称，场景和备注可选；确认场景就绪后开始。默认快捷键为 Ctrl+Shift+F9；定时停止默认关闭。
5. 停止后保持游戏运行，直到界面提示可以关闭。工具串行完成排空、转换和校验，原始 stream 保留。
6. 在结果页打开 Viewer、文件夹，或复制分析请求。Viewer 路径可选；复制请求不会调用 AI。

第一版仅支持本机。连接已有目标不会应用所选 INI；实际配置来自最终 Trace。管理员启动不代表 Sampling / Context Switch 一定有效，最终以校验证据为准。

## 状态、关闭和恢复

任务根目录保存 `task.json`、请求 INI 快照、stream、正式 Trace，以及按尝试编号隔离的进程日志和 MCP 请求/响应。应用设置在 `%LOCALAPPDATA%\JNTracyCapture`。

- 正常停止统一使用 stop-file。连接中的停止表示取消连接。
- 忙碌时关闭窗口会提供返回、后台继续、强制关闭三项。后台通过托盘恢复。
- 强制关闭只结束本工具拥有的工作进程，不结束游戏或 Editor；本轮标记为中断。
- 从录制记录中选择恢复或重试。身份匹配的存活 Capture / converter 可接管；Query 使用本次新会话。
- 失败或中断文件不能视为正式输入。仅验证成功的候选可发布；数据域的质量问题单独列出。
- 原始 stream 改变时，旧候选和校验证据失效。不会覆盖已有正式文件。

转换使用 `name.pending.tracy` 作为候选，适配 Query 的 `.tracy` 扩展名检查。校验后改为 `name.tracy`，并更新 stream 的 snapshot-map。候选文件不可提前交给正式分析。

## 构建与测试

需要 Visual Studio 2026 的 x64 C++ 工具链、Windows SDK、CMake 3.25+。依赖版本与仓库一致；支持 CPM 本地源码覆盖。

```powershell
cmake -S capture-gui -B build/gui -A x64
cmake --build build/gui --config Release
ctest --test-dir build/gui -C Release --output-on-failure
```

`scripts/Build-Package.ps1` 构建并打包 GUI、Capture、converter 和 Query，生成文件 SHA-256 清单。构建工具和打包脚本不属于运行时依赖。

集成测试使用 `capture-gui-workflow-runner` 测试程序驱动真实 Controller，并连接仓库 `tracy-stream-test-producer`；不会调用 AI，不把模拟数据宣称为 Unity 实测。

```powershell
python capture-gui/tests/test_capture_cli.py <tracy-capture.exe>
python capture-gui/tests/test_workflow.py <workflow-runner.exe> <tool-directory> <test-producer.exe> <output-directory>
```

真实 Player / Editor 场景、UAC、托盘、快捷键及多 DPI 显示需要在交互式桌面另外验收。自动化流水线测试不能替代这些结果。

## 接口兼容

Capture 增加 `--headless` 和 `--status-json <path>`；旧命令行保持可用。状态包含 schema、state、first_data_received、client_pid、elapsed_ms、session_end。终态日志保留，最终完整性必须结合退出码和 stream 校验，不能只看状态文件。

INI 热切换、远程录制、批量任务、AI 自动分析不在第一版范围。
