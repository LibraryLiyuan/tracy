# 构建部署与跨机器复现

构建和部署需要以同一套源码、包映射、资源契约和符号为单位。目录名或相同 Unity 公共版本号不能证明产物兼容。

## 1 Tracy 构建

Tracy 的 Query、Profiler、Capture、stream 和 Capture GUI 有各自 CMake 入口。下面为从指定仓库构建的示例，首次配置应选择本机实际 Visual Studio、SDK 和依赖路径，并记录配置：

```powershell
Set-Location 'C:\CodeProjects\GodotProjects\tracy-0.13.1'
cmake -S query -B build-handoff/query -G 'Visual Studio 18 2026' -A x64
cmake --build build-handoff/query --config Release --target tracy-query tracy-analyze --parallel 4
cmake -S profiler -B build-handoff/profiler -G 'Visual Studio 18 2026' -A x64
cmake --build build-handoff/profiler --config Release --parallel 4
cmake -S capture-gui -B build-handoff/capture-gui -G 'Visual Studio 18 2026' -A x64
cmake --build build-handoff/capture-gui --config Release --parallel 4
```

这些命令是复现入口，本轮没有执行构建。独立构建其他模块时参照相应 CMakeLists，不把某一模块成功当作整套工具成功。产物集中交付时记录所有 EXE/DLL/PDB 的 SHA、source HEAD/dirty patch、编译器、Release/Debug 和 LTO 设置。[CMakeLists.txt:1](../query/CMakeLists.txt#L1) [CMakeLists.txt:1](../profiler/CMakeLists.txt#L1) [CMakeLists.txt:1](../capture/CMakeLists.txt#L1)

mainline 源码会生成 16 GiB guard 的 analyzer；10 月 13 日安装的 32 GiB 修订来自另外的 dev 基线与补丁。按哪套重现必须提前写明。

## 2 Unity 构建入口

仓库基于 Bee；jam 名称是历史入口。常用目标为 JNTracyClient、WinEditor、WinPlayerIL2CPP。优先使用当前仓库与本机匹配的入口，不把旧机器路径的 build cache 混用。

```powershell
Set-Location 'C:\workflow\jnunity'
& 'C:\Perl64\bin\perl.exe' jam.pl JNTracyClient -sCONFIG=release
.\jam WinEditor -sCONFIG=release
.\jam WinPlayerIL2CPP
```

Perl 示例来自已保存 Client 补丁流程，本机位置不同时替换。独立 Client 目标生成 DLL、Harness 并部署配置；Unity 主程序不应再次编译 Client Private 实现。[JNTracyClient.jam.cs:16](C:/workflow/jnunity/Projects/Jam/JNTracyClient.jam.cs:16) [RuntimeFiles.jam.cs:245](C:/workflow/jnunity/Projects/Jam/RuntimeFiles.jam.cs:245)

## 3 选择替换模式

| 修改范围 | 构建与替换范围 |
|---|---|
| 引擎 Native 实现，ABI/binding/资源契约不变 | 对应 Native 产物与匹配 PDB |
| 项目或 Package C#，序列化契约不变 | Scripts-only 的 GameAssembly、metadata、Burst、初始化清单与符号闭包 |
| Managed API、binding 或引擎与项目共同改变 | 使用同源 Editor 的协同构建，成套替换引擎与脚本 |
| 明确需要整套 runtime | 完整产物矩阵并独立完整 Player 副本 |
| shader、scene、序列化布局、模块/stripping 或资源 schema 改变 | 重新判断是否需要内容或完整 Player 构建，不能继续伪装 code-only |

实际规则见 staging Skill；它附带的 PowerShell 脚本仅支持 Full-runtime，不支持所有上述模式。[SKILL.md:1](C:/Users/Admin/.codex/skills/jntracy-stage-player-code/SKILL.md:1)

## 4 最小 Client 补丁案例

10 月 12 日针对录制崩溃，确认公共接口、ABI、ConfigHash 与内嵌 Tracy 不变后，仅构建和替换 JNTracyClient.dll/PDB，并复用 UnityPlayer、GameAssembly 与资源。整次 merge 相对旧包还有 Animator API 等其他变化；只修 Client 不表示整次 merge 都已部署。

该案例还发现实际 DLL 与旧 manifest 不同、PDB 不匹配，因此补丁前先备份和核对 PE/PDB，再形成新身份。后续有 5 秒后台录制、转换、索引和结构校验，完整前台与长时间验证仍未完成。[deployment-result.json:1](C:/Users/Admin/Documents/JN-Unity-T3/artifacts/client-patch-20261012-115800-c0cde1b6/deployment-result.json:1)

## 5 完整独立 Player 的闭环

1. 记录原包清单、资源与插件、引擎/ProductVersion、项目版本、实际 Package 解析和 Git/P4 本地状态。
2. 备份临时项目设置与配置；确认 scripts-only 的标志确实到达 BuildOptions.BuildScriptsOnly。
3. 使用同源构建产物，检查最终退出码、完整成功与 post-build 校验，不能只凭日志中间的 Success。
4. 创建原包的完整独立副本，再按模式替换闭包。build seed 只是构建辅助，不能拿来当运行包。
5. 校验替换文件、保留资源、唯一 Client DLL、PDB 配对与原包未变化；恢复临时设置。
6. 运行验证包括登录、代表性资源加载与渲染、Tracy 连接、effective config 和正常退出。

原生插件不能因版本看起来更新就全部覆盖。GameAssembly 与 metadata/Burst 不能混代。序列化布局检查失败应解决真实契约，不应关闭检查掩盖错误。

## 6 P4 和 Package 映射

Unity 构建/导入可能改写本地 Assets、.meta、材质或 ProjectSettings，即使没有执行 p4 edit/submit。历史已有“未操作服务器，但污染本地 workspace”的纠正案例。构建前后需要记录实际差异，不能凭未执行 P4 命令声明没有本地变化。

实际包来源以 manifest、packages-lock、Editor 启动日志和构建记录为准。2375 曾使用兼容音频/渲染包映射；2453 有新独立包。它们不能简化为“当前 PackageRepo HEAD 的所有代码”。

## 7 交付包应包含

工具与版本清单、Player 部署 manifest、DLL/PDB 身份、INI 与实际配置样例、Skill 目录及机器配置模板、一个可分享的小型验证 capture/store、说明文档、测试输出、回滚副本与已知例外。

复现先用小样本验证整条链，再处理大型录制。编译成功、静态部署通过、真实业务通过和性能达标分别记录；当前历史 2453 包的完整前台稳定性仍需后续证明。
