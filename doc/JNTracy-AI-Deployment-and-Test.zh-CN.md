# JN Unity × Tracy AI 性能分析：三仓部署与同事验收指南

## 1. 文档目标

本文用于在一台新的 Windows 开发机上部署以下三套公司 Git 仓库，并完成一次可由 Codex Skill 和 Tracy Query/MCP 分析的真实 Trace 验收：

```text
jnunity
+ PackageRepo
+ JN Tracy fork
→ Unity Editor/Player 产生 HighEvidence 数据
→ tracy-capture 写入 .tracy-stream
→ tracy-stream-convert 转换 .tracy
→ tracy-query --mcp 提供结构化证据
→ diagnose-jn-unity-tracy 生成 Markdown 分析报告
```

本流程固定支持 Windows x64、D3D12、Graphics Jobs Off。Skill 分析单个 Trace，不执行多次运行 A/B，也不会自动修改项目、引擎或 Tracy 源码。

## 2. 已验证的版本合同

| 组件 | 公司 Git | 目标分支 | 本文编写时验证 HEAD |
|---|---|---|---|
| Unity 引擎源码 | `git@code.byted.org:jng/jnunity.git` | `feature/JNTracy` | `8df2ef2b1e658f4c3d7c0c155f06398b80c64c18` |
| Unity Package | `git@code.byted.org:jng/jnunity-packages.git` | `feature/Pack-JNTracy` | `313489df3489e9e0fd580baab8b76d6f97b03e9c` |
| Tracy fork、工具和 AI Skill | `git@code.byted.org:jng/tracy.git` | 合并目标 `dev`；评审分支 `feature/JNTracy-AI-Skill-v1.1.2` | 当前发布候选使用评审分支；合并后使用 `jntracy-ai-skill-v1.1.2` 标签或其后续 `dev` |

运行时合同：

| 项目 | 要求 |
|---|---:|
| Tracy Protocol | 90 |
| JN trace section | 12 |
| Query schema | 1.32.0 |
| Query index | 13 |
| GPU Resource Catalog schema | 1 |
| Skill | `diagnose-jn-unity-tracy` 1.1.2 |

不要混用其他分支或旧目录中的 `tracy-capture.exe`、`tracy-stream-convert.exe`、`tracy-query.exe` 和 `tracy-profiler.exe`。四个工具必须来自同一次 Tracy 源码身份和同一 Release 构建。

## 3. 前置软件与权限

安装并确认：

- Windows 10/11 x64。
- Visual Studio 2022，包含“使用 C++ 的桌面开发”和 Windows SDK。
- CMake 3.25 或更高版本。
- Git，并已配置公司 Git SSH 权限。
- PowerShell 7。
- Codex Desktop。
- Python 3；报告脚本只使用标准库，不需要 pip 安装第三方依赖。
- D3D12 驱动和能够正常运行目标 Editor/Player 的项目环境。

Sampling 与 Context Switch 需要以管理员权限运行 capture/目标进程。普通 Query、转换和离线报告生成不需要管理员权限。

## 4. 克隆三个仓库

以下路径只是示例，可以换成个人目录；最终路径必须写入机器私有的 `local-profile.json`。

```powershell
$root = 'C:\JNTracy'
New-Item -ItemType Directory -Path $root -Force | Out-Null

git clone --branch feature/JNTracy --single-branch `
  git@code.byted.org:jng/jnunity.git `
  "$root\jnunity"

git clone --branch feature/Pack-JNTracy --single-branch `
  git@code.byted.org:jng/jnunity-packages.git `
  "$root\PackageRepo"

git clone --branch feature/JNTracy-AI-Skill-v1.1.2 --single-branch `
  git@code.byted.org:jng/tracy.git `
  "$root\tracy"
```

身份检查：

```powershell
git -C "$root\jnunity" status --short --branch
git -C "$root\PackageRepo" status --short --branch
git -C "$root\tracy" status --short --branch

git -C "$root\jnunity" rev-parse HEAD
git -C "$root\PackageRepo" rev-parse HEAD
git -C "$root\tracy" rev-parse HEAD
```

开始编译前，三个工作区都应没有未知 tracked 修改。构建目录、`.codegraph` 等本地产物不得提交。

## 5. 编译 Protocol 90 Tracy 工具链

从“Developer PowerShell for VS 2022”执行：

```powershell
$tracy = 'C:\JNTracy\tracy'
$build = Join-Path $tracy 'build-team'
$release = Join-Path $tracy 'build-latest\Release'

cmake -S "$tracy\profiler" -B "$build\profiler" `
  -G 'Visual Studio 17 2022' -A x64
cmake --build "$build\profiler" --config Release --target tracy-profiler

cmake -S "$tracy\capture" -B "$build\capture" `
  -G 'Visual Studio 17 2022' -A x64 `
  -DNO_STATISTICS=OFF
cmake --build "$build\capture" --config Release `
  --target tracy-capture tracy-stream-convert tracy-stream-inspect

cmake -S "$tracy\query" -B "$build\query" `
  -G 'Visual Studio 17 2022' -A x64 `
  -DBUILD_TESTING=ON
cmake --build "$build\query" --config Release `
  --target tracy-query tracy-query-contract-tests tracy-gpu-analysis-tests tracy-query-mcp-transcript-tests

cmake -S "$tracy\stream" -B "$build\stream" `
  -G 'Visual Studio 17 2022' -A x64 `
  -DBUILD_TESTING=ON
cmake --build "$build\stream" --config Release `
  --target tracy-stream tracy-stream-journal-tests

New-Item -ItemType Directory -Path $release -Force | Out-Null
Copy-Item "$build\profiler\Release\tracy-profiler.exe" $release -Force
Copy-Item "$build\capture\Release\tracy-capture.exe" $release -Force
Copy-Item "$build\capture\Release\tracy-stream-convert.exe" $release -Force
Copy-Item "$build\capture\Release\tracy-stream-inspect.exe" $release -Force
Copy-Item "$build\query\Release\tracy-query.exe" $release -Force
Copy-Item "$build\stream\Release\tracy-stream.exe" $release -Force
```

运行工具门禁：

```powershell
& "$release\tracy-query.exe" --version
& "$release\tracy-query.exe" --doctor

ctest --test-dir "$build\query" -C Release --output-on-failure
ctest --test-dir "$build\stream" -C Release --output-on-failure
```

如果任何 Query/Stream 测试失败，不要继续捕获。不要只替换其中一个 EXE。

## 6. 编译 Unity 接入

先确认 PackageRepo 的 `feature/Pack-JNTracy` 已被项目实际引用。然后在引擎仓库执行：

```powershell
Set-Location 'C:\JNTracy\jnunity'
.\jam JNTracyClient
```

需要 Editor 验证时继续执行项目现有的 WindowsEditor 增量构建。需要 Player 验证时，应使用项目正式流水线生成或用已批准的 Player code staging 流程替换完整代码产物，禁止仅替换 IL2CPP 的 `Assembly-CSharp.dll`。

至少保持下列二进制和符号同源：

```text
UnityPlayer.dll/.pdb
JNTracyClient.dll/.pdb
GameAssembly.dll/.pdb
global-metadata.dat
tracy-capture/query/converter/profiler.exe
```

## 7. 安装 AI 分析 Skill

Tracy 仓库中的目录是 Skill 权威源码：

```text
C:\JNTracy\tracy\skills\diagnose-jn-unity-tracy
```

安装到当前 Windows 用户：

```powershell
$skillSource = 'C:\JNTracy\tracy\skills\diagnose-jn-unity-tracy'
& "$skillSource\scripts\install-skill.ps1"
```

首次安装会创建：

```text
%USERPROFILE%\.codex\skills\diagnose-jn-unity-tracy\config\local-profile.json
```

编辑该文件，把示例路径替换为本机真实路径。`local-profile.json` 是机器私有文件，不得提交；共享配置和预算不要复制机器路径。

更新 Skill：

```powershell
git -C 'C:\JNTracy\tracy' pull --ff-only
& 'C:\JNTracy\tracy\skills\diagnose-jn-unity-tracy\scripts\install-skill.ps1' -Force
```

`-Force` 必须保留已有的 `config\local-profile.json`。安装后重启 Codex Desktop，使新 Skill 被重新发现。

## 8. 验证 Skill 与本机配置

```powershell
$skill = "$env:USERPROFILE\.codex\skills\diagnose-jn-unity-tracy"

& "$skill\scripts\resolve-profile.ps1" `
  -OutputPath "$env:TEMP\jntracy-resolved-profile.json"

& "$skill\tests\test-distribution.ps1"
& "$skill\tests\test-scripts.ps1"
```

期望：

- `resolve-profile.ps1` 找到四个同源 Tracy 工具、Python 和三个源码仓库。
- Distribution 测试证明安装和升级不会覆盖本地配置。
- PowerShell 与 Python 回归测试全部通过。
- `project-profile.json` 显示 Protocol 90、Query 1.32.0、section 12、Catalog 1。

## 9. 第一次 AI 分析建议

先用一个已知可打开的 `.tracy` 完成离线链路，再做真实录制。这样可以把“工具/Skill 部署问题”和“游戏录制问题”分开。

在 Codex 中输入：

```text
使用 $diagnose-jn-unity-tracy 分析：
C:\JNTracyData\PlayerCaptures\KnownGood.tracy

要求分析整个 Trace，使用常驻 Tracy Query MCP，生成 Markdown 主报告和静态 SVG/PNG 证据；HTML 保持关闭。先执行身份、协议、覆盖率和数据质量验证，Catalog 无效时不得伪造资源级结论。
```

最小验收结果：

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
└─ visuals/assets/static-previews/
```

`report-validation.json` 必须为 `passed=true` 且 `error_count=0`。没有坐标、typed relation 或资源节点时，报告应写明 EvidenceGap，不应生成空白占位 SVG。

## 10. 真实 HighEvidence 捕获

1. 确认目标 Editor/Player 使用与三个源码仓库匹配的 JNTracyClient。
2. 确认 `JNTracy.HighEvidence.ini` 位于 Player/Editor 约定目录。
3. 需要 Sampling/Context Switch 时，以管理员权限启动目标和 `tracy-capture.exe`。
4. 手动启动 capture，确认窗口可见，不要用隐藏窗口。
5. 游戏进入稳定场景后开始保留有效窗口。
6. 结束时只按一次 `Ctrl+C`，等待 capture 自行落盘和退出；不要连续按两次。
7. 先保存 `.tracy-stream` 原件，再离线转换：

```powershell
& 'C:\JNTracy\tracy\build-latest\Release\tracy-stream-convert.exe' `
  -i 'C:\JNTracyData\PlayerCaptures\Run01.tracy-stream' `
  -o 'C:\JNTracyData\PlayerCaptures\Run01.tracy'
```

8. 用同目录的 `tracy-profiler.exe` 打开 `.tracy`，同时让 Skill 使用同目录的 `tracy-query.exe` 做 MCP 分析。

不要删除 `.tracy-stream`；它是转换异常、长 Trace 和数据一致性排查的原始证据。

## 11. AI 报告人工复核

至少人工核对：

- Trace 身份、目标进程、场景、分辨率和 HighEvidence 配置。
- Main、Render、Job Worker、GPU Direct/Compute/Copy 的线程与 Queue。
- 报告中的代表帧、Zone 时长和 Primary Limiter。
- Job Schedule/Ready/Slice/Complete/Wait 关系。
- GPU Pass 层级、显存总量和 GPU Catalog 状态。
- Evidence Card 引用的调用栈、源码和 FrameImage。
- Tracy Runtime/Sampling/写盘是否进入关键路径。

Markdown 是权威报告；SVG/PNG 是证据附件。Profiler GUI 用于人工交叉检查，不能替代 Query/MCP 的结构化验证。

## 12. 常见故障

| 现象 | 处理 |
|---|---|
| Codex 找不到 Skill | 确认安装目录和 `SKILL.md`，重启 Codex Desktop |
| `resolve-profile` 找不到工具 | 修正机器私有 `local-profile.json`，不要修改共享 project profile |
| Protocol/section/query schema 不一致 | 删除混用工具，重新从同一 Tracy commit 成套构建 |
| Profiler 能开但 MCP 域缺失 | 先检查 Query capability、Trace quality 和转换完整性 |
| GPU Catalog `invalid_catalog_generation` | 只允许分析 DXGI/物理总量；资源名称、Range 和 Pass 来源必须标 EvidenceGap |
| 没有 C#/Lua 精确栈 | 检查 HighEvidence 的 Source Stack 配置和对应 managed/Lua producer capability |
| 没有 Sampling/Context Switch | 以管理员权限重新捕获并确认 Windows 权限状态 |
| `.tracy-stream` 转换很慢 | 保留原件，检查记录数量、转换进度和工具身份；不要用旧 converter |
| 报告有空 SVG | 这是 Skill 回归；运行 1.1.2 测试并确认没有占位图 |

## 13. 更新与回滚

更新前记录三个仓库 HEAD、二进制 SHA-256、INI SHA-256 和 Skill 版本。更新使用 `git pull --ff-only`，发生非快进或冲突时停止，不能自动覆盖本地修改。

Skill 回滚可以直接检出 Tracy 发布标签，然后重新安装：

```powershell
git -C 'C:\JNTracy\tracy' switch --detach jntracy-ai-skill-v1.1.2
& 'C:\JNTracy\tracy\skills\diagnose-jn-unity-tracy\scripts\install-skill.ps1' -Force
```

安装器会保留机器私有 `local-profile.json`。评审分支合并后，如需回到 `dev`：

```powershell
git -C 'C:\JNTracy\tracy' switch dev
```

Trace 可能包含项目 Marker、资源名称、源码路径、调用栈和 FrameImage，只能存放在公司批准的位置，不得上传公共服务。
