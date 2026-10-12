# diagnose-jn-unity-tracyV2 备份

当前 skill 完整备份，内部版本 **2.6.3**；备份目录名为用户指定的 diagnose-jn-unity-tracyV2。SKILL.md 内的原始技能名称和版本保留，未自动安装为第二个技能。

## 内容

- `SKILL.md`、scripts、references、config、schemas、tests、assets、agents、docs、vendor 与原 README：来自当前已安装技能。
- `bin/tracy-analyze.exe`：已验证的原生 stream 分析器，分类器 frame-triage-v4.1。
- `bin/tracy-profiler-offscreen.exe`：当前已验证的原生查看器，包含固定离屏画布与网页交互支持。
- `tools/stream-report-viewer/`：当前 Python 服务和 HTML/JS、测试，包含 1:1 清晰显示、2×整数放大、适应宽度及刷新保留定位。最新字体清晰度修复位于这里，未因此重新编译 EXE。
- `backup-provenance/`：原始本机配置、源文件哈希清单、备份记录、分析器说明和验证结果。
- `backup-manifest.json`：备份全部文件的 SHA-256 与大小（清单本身除外）。

## 版本与完整性

| 文件 | SHA-256 |
| --- | --- |
| tracy-analyze.exe | 49c7a53af1a70878f3410a2cab113bbcf695f5928cb371fe6edd4de137e6eedd |
| tracy-profiler-offscreen.exe | 9c18b11a105016a5a373a4c766c68f6c6dd3744925f53c4718323b37291c5288 |

原分析器配置引用的 F 盘旧录制目录已不存在。本次从 E:/tracy/_build-all/query-dev/Release 找到字节相同的 EXE，哈希与配置中的已验证身份一致。

除下面两个本机路径配置外，当前安装技能的所有文件逐文件保留并核验哈希；这两个配置的原件保存在 backup-provenance。网页工具不包含可重新生成的 __pycache__ / pyc。

## 使用与恢复

`config/stream-local.json` 与 `config/stream-viewer-local.json` 已指向此备份内的 bin 和 tools，因此不再依赖原 EXE 所在录制目录。移动备份到其他位置后，需要同步修改这两个配置中的绝对路径。

在此目录执行：

```powershell
python -X utf8 scripts/lint-skill.py
.\bin\tracy-analyze.exe --help
python -X utf8 scripts/stream_workflow.py --help
python -X utf8 tools/stream-report-viewer/prepare.py --help
```

实际分析仍需外部捕获文件与已发布的 EventStore；源码深查仍需 Unity、Tracy、客户端 C#/Lua 源码。网页工具依赖 Python 3.11+；Node 仅用于显示测试。没有复制大型录制、分析报告、完整 C++ 源码或 Python/Node 运行时。现有报告的 viewer-config.json 是独立登记文件，复用它时应核对其 EXE 路径与哈希；生成新报告按 references/stream-native-views.md 操作。

旧普通 .tracy 分支的 local-profile.json 仍是原始模板；此次随附 EXE 和本机可用配置针对当前 .tracy-stream 工作流，不宣称已配置旧 MCP 工作流。

本次只创建备份，未修改当前已安装技能或正在运行的网页服务。尚未提交 Git。
