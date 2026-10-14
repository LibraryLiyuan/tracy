# 采集配置矩阵

本表直接来自指定引擎 HEAD 的十份 INI。未设置表示文件没有显式字段，需要结合实现默认和 effective config；不表示关闭，也不认证某个 Player 的实际配置。

## 档位和系统采样

| 文件 | Preset / Profile | Sampling / Hz | ContextSwitch / Scope |
|---|---|---|---|
| JNTracy.AllDomainsStress.ini | AllDomainsStress / Detail | true / 2000 | true / Process |
| JNTracy.Daily.ini | Daily / PassAndPhase | true / 1000 | true / Process |
| JNTracy.HighEvidence.ini | HighEvidence / Detail | true / 2000 | true / Process |
| JNTracy.ini | HighEvidence / Detail | true / 2000 | true / Process |
| JNTracy.Triggered.ExactCallstack.ini | Triggered.ExactCallstack / Triggered | false / 2000 | false / Process |
| JNTracy.Triggered.GpuReference.ini | Triggered.GpuReference / Triggered | false / 1000 | false / Process |
| JNTracy.Triggered.GpuReference.Periodic60.ini | Triggered.GpuReference.Periodic60 / Detail | true / 2000 | true / Process |
| JNTracy.Triggered.LuaCoroutine.ini | Triggered.LuaCoroutine / Triggered | true / 2000 | true / Process |
| JNTracy.Triggered.StartupIOMemory.ini | Triggered.StartupIOMemory / Triggered | true / 1000 | true / Process |
| JNTracy.Triggered.SystemSampling.ini | Triggered.SystemSampling / LowLevel | true / 8000 | true / Process |

## GPU 与资源

| 文件 | Reference Mode / Granularity | Catalog | Detailed Evidence |
|---|---|---|---|
| JNTracy.AllDomainsStress.ini | Disabled / 未设置 | true | Periodic |
| JNTracy.Daily.ini | Disabled / 未设置 | false | Disabled |
| JNTracy.HighEvidence.ini | Enabled / 未设置 | true | Disabled |
| JNTracy.ini | Enabled / Frame | false | Disabled |
| JNTracy.Triggered.ExactCallstack.ini | Disabled / 未设置 | false | Disabled |
| JNTracy.Triggered.GpuReference.ini | Enabled / 未设置 | true | Manual |
| JNTracy.Triggered.GpuReference.Periodic60.ini | Enabled / 未设置 | true | Periodic |
| JNTracy.Triggered.LuaCoroutine.ini | Disabled / 未设置 | false | Disabled |
| JNTracy.Triggered.StartupIOMemory.ini | Disabled / 未设置 | false | Disabled |
| JNTracy.Triggered.SystemSampling.ini | Disabled / 未设置 | false | Disabled |

## 调用栈和图像

| 文件 | UnityMarker / Job / GPU Depth | CSharpSource / LuaSource | FrameImage / 间隔帧 |
|---|---|---|---|
| JNTracy.AllDomainsStress.ini | 8 / 8 / 8 | 8 / 8 | true / 60 |
| JNTracy.Daily.ini | 0 / 8 / 0 | 8 / 8 | true / 60 |
| JNTracy.HighEvidence.ini | 0 / 8 / 8 | 8 / 8 | true / 60 |
| JNTracy.ini | 0 / 8 / 8 | 8 / 8 | true / 60 |
| JNTracy.Triggered.ExactCallstack.ini | 8 / 8 / 8 | 8 / 8 | false / 60 |
| JNTracy.Triggered.GpuReference.ini | 0 / 8 / 8 | 8 / 8 | true / 10000 |
| JNTracy.Triggered.GpuReference.Periodic60.ini | 8 / 8 / 8 | 8 / 8 | true / 60 |
| JNTracy.Triggered.LuaCoroutine.ini | 8 / 8 / 0 | 8 / 8 | true / 10000 |
| JNTracy.Triggered.StartupIOMemory.ini | 0 / 8 / 0 | 8 / 8 | false / 60 |
| JNTracy.Triggered.SystemSampling.ini | 0 / 0 / 0 | 0 / 0 | false / 60 |

全部字段与 SHA 见 [capture-configs.json](../evidence/capture-configs.json)。Preset 不是严格超集；Depth 与 SiteReuse/PerEventExact、Source 开关和预算共同影响实际证据。

默认 JNTracy.ini 与独立 HighEvidence 同名不同内容。启动覆盖与 GUI 检查见 [采集操作](../05-capture-and-autologin.zh-CN.md)。
