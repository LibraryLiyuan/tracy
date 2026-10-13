#ifndef __TRACYJNUSAGE_HPP__
#define __TRACYJNUSAGE_HPP__

#include <stdint.h>

// CPU, memory and GPU usage of the machine and of the recorded process. While
// a server is connected, a background thread of the JN engine samples them
// every 100 ms ([SystemUsage] IntervalMs) and sends these plots.
//
// A percentage is the average over the interval that ends at its point, which
// starts at the previous point of the plot; the first point of a connection
// covers one interval. Bytes are the value at the point.
//
// CPU busy time comes from the processors' idle cycle counters and the
// process's cycle count, measured against the time stamp counter, so it is
// exact rather than counted in scheduler ticks. A percentage of the machine is
// a share of all logical processors.

namespace tracy
{
namespace JnUsage
{

// Busy share of all logical processors, and the process's share of them.
constexpr const char* CpuSystemPlot = "CPU.System.Percent";
constexpr const char* CpuProcessPlot = "CPU.Process.Percent";
// One plot per logical processor: "CPU.Core.<index>.Percent", the index in
// two digits (three on machines with more than 100 logical processors).
constexpr const char* CpuCorePrefix = "CPU.Core.";
constexpr const char* CpuCoreSuffix = ".Percent";
constexpr int MaxCores = 512;

// The process's private commit and working set, and the machine's physical
// memory in use and installed.
constexpr const char* MemoryProcessCommittedPlot = "Memory.Process.CommittedBytes";
constexpr const char* MemoryProcessWorkingSetPlot = "Memory.Process.WorkingSetBytes";
constexpr const char* MemorySystemUsedPlot = "Memory.System.UsedBytes";
constexpr const char* MemorySystemTotalPlot = "Memory.System.TotalBytes";

// The engines of the GPU adapter the process renders with. Each plot is the
// share of time the busiest engine of its kind ran the process's work; only
// the kinds the adapter has are sent.
enum class Engine : uint8_t
{
    ThreeD,
    Compute,
    Copy,
    VideoDecode,
    VideoEncode,
    NUM
};
constexpr int Engines = int( Engine::NUM );
constexpr const char* EngineNames[Engines] = { "3D", "Compute", "Copy", "Video decode", "Video encode" };
constexpr const char* GpuEnginePlots[Engines] = {
    "GPU.Process.3D.Percent", "GPU.Process.Compute.Percent", "GPU.Process.Copy.Percent",
    "GPU.Process.VideoDecode.Percent", "GPU.Process.VideoEncode.Percent" };
// Plot colors, 0xRRGGBB.
constexpr uint32_t EngineColors[Engines] = { 0x76B900, 0xE0B040, 0x5185CE, 0xBA6EAC, 0xEE7744 };

// The adapter's memory over all processes: bytes resident in its video
// memory segments and their size, and bytes resident in system memory
// segments. The process's own use is GPU.DXGI.Local.UsageBytes (TracyJnVram).
constexpr const char* GpuDedicatedUsedPlot = "GPU.Adapter.Dedicated.UsedBytes";
constexpr const char* GpuDedicatedTotalPlot = "GPU.Adapter.Dedicated.TotalBytes";
constexpr const char* GpuSharedUsedPlot = "GPU.Adapter.Shared.UsedBytes";

// The display, from DXGI's frame statistics of the swap chain, which the
// engine already reads once a frame while VSync is on. A point is at the
// refresh (vertical blank) a new frame appeared at, and its value is the
// number of refreshes since the frame before it appeared: 1 when the frame
// was ready in time, 2 when it missed a refresh and the frame before stayed
// on screen for two, and so on.
constexpr const char* DisplayRefreshesPlot = "Display.Refreshes";
// Rarely, more than one frame appeared since the previous point; then a point
// of this plot at the same time gives how many, and the refreshes cover them
// all.
constexpr const char* DisplayFramesPlot = "Display.Frames";

// The processors, as an app info message, which Tracy repeats to every
// connection: "JNTracy.Usage cpus=<logical processors> classes=<efficiency
// class of each, comma separated> interval_ms=<sampling interval>". Classes
// are Windows' EfficiencyClass: on a hybrid CPU the lowest class is the
// efficiency cores.
constexpr const char* AppInfoPrefix = "JNTracy.Usage ";

}
}

#endif
