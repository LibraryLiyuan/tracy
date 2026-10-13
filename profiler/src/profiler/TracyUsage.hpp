#ifndef __TRACYUSAGE_HPP__
#define __TRACYUSAGE_HPP__

#include <array>
#include <functional>
#include <optional>
#include <stdint.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "TracyVram.hpp"
#include "../../../public/common/TracyJnUsage.hpp"

namespace tracy
{

// The CPU, memory and GPU usage plots of a capture (TracyJnUsage.hpp). A
// percentage point is the average over the interval since the plot's previous
// point; bytes hold until the next point. The engine samples every 100 ms, so
// a 30 minute capture holds about 18,000 points per plot and they are loaded
// whole.
struct UsageData
{
    VramSeries cpuSystem, cpuProcess;
    // Tracy's own CPU usage of the machine: the CPU lane of captures that
    // predate the engine's plots.
    VramSeries tracyCpu;
    // Per logical processor, and Windows' efficiency class of each; empty
    // when the capture has no processor description.
    std::vector<VramSeries> cores;
    std::vector<int> coreClasses;
    VramSeries processCommitted, processWorkingSet, systemUsed, systemTotal;
    // Unity's counters of the process's memory, for captures that predate
    // the engine's plots.
    VramSeries unityCommitted, unityResident;
    std::array<VramSeries, JnUsage::Engines> engines;
    // The engines the lane shows: 3D, and the others that ran the game's
    // work at some point.
    std::array<bool, JnUsage::Engines> engineShown {};
    // The display plots, and from them the frames the display showed: each
    // from the refresh the frame before it appeared at (or the earliest it
    // could have) to the refresh it appeared at. A point that covers several
    // frames is one entry.
    VramSeries displayRefreshes, displayFrames;
    struct DisplayFrame
    {
        int64_t start, end;
        uint32_t refreshes, frames;
        // The refreshes the frames waited past one each.
        uint32_t Missed() const { return refreshes > frames ? refreshes - frames : 0; }
    };
    std::vector<DisplayFrame> display;
    // The display's refresh period, from the frames that took one refresh.
    int64_t refreshPeriod = 0;
    // The sampling interval, from the processor description or the plots.
    int64_t interval = 100'000'000;
    int64_t lastTime = 0;
    // The lanes' pixel columns of each series. The lanes show the whole
    // capture, so these stay until the lane's width or the capture's range
    // change.
    struct Columns
    {
        int64_t begin = 0;
        double pxns = 0;
        std::vector<double> values;
    };
    mutable std::unordered_map<const VramSeries*, Columns> columns;

    // The series a plot of this name loads into, null for other plots.
    VramSeries* Find( std::string_view name );
    // Takes the processor description from an app info message, if it is one.
    void ParseAppInfo( std::string_view text );
    // Orders the series by time once loaded and settles the fallbacks.
    void Finish();
    // Builds the display frames from the display plots.
    void FinishDisplay();
    bool HasCpu() const;
    bool HasCores() const;
    bool HasMemory() const;
    bool HasGpu() const;
    bool HasDisplay() const { return !display.empty(); }
    bool Empty() const { return !HasCpu() && !HasMemory() && !HasGpu() && !HasDisplay(); }
    // Whether the processors have two core types, and whether one is an
    // efficiency core.
    bool Hybrid() const;
    bool EfficiencyCore( size_t core ) const;
};

// The usage plots, which the usage lanes draw instead of a plot lane each.
bool IsUsagePlot( std::string_view name );

enum class UsageLane : uint8_t
{
    // The machine's and the process's CPU busy share, then one row per
    // logical processor.
    Cpu,
    // The process's memory, then the machine's.
    Memory,
    // The process's use of each GPU engine.
    Gpu,
    // The frames the display showed against its refreshes (VSync).
    Display,
    NUM
};
constexpr int UsageLanes = int( UsageLane::NUM );

// The time the GPU's direct queue was busy in a range, for the display
// lane's tooltip: the union of its top-level zones there, approximate when
// some were folded together. Empty when the zones there are not loaded.
struct GpuBusy
{
    int64_t busy = 0;
    bool approximate = false;
};
using GpuBusyFn = std::function<std::optional<GpuBusy>( int64_t begin, int64_t end )>;

const char* UsageLaneLabel( UsageLane lane );
bool UsageLanePresent( const UsageData& data, UsageLane lane );
// The lane's height below its header, from Tracy's plot height.
float UsageLaneHeight( const UsageData& data, UsageLane lane, float plotHeight );
// Draws a lane in `area`, as tall as UsageLaneHeight gives for `plotHeight`,
// with a legend of what it shows on the header row (VramPlotArea::legendMinX).
// Returns the time under the mouse, after showing its tooltip.
std::optional<int64_t> DrawUsageLane( ImDrawList* draw, const VramPlotArea& area, const UsageData& data, UsageLane lane, float plotHeight,
    const GpuBusyFn& gpuBusy = {} );
// The header tooltip of a lane: its peaks and averages over the capture.
void UsageLaneTooltip( const UsageData& data, UsageLane lane );

// The frames of a frame set, for the FPS lane: starts in time order, and each
// frame's end, -1 where it has none; without ends a frame ends where the next
// starts. The last frame of a continuous set, still open, does not count.
struct FrameTimes
{
    const std::vector<int64_t>* starts = nullptr;
    const std::vector<int64_t>* ends = nullptr;
    int64_t lastNs = 0;
    // The number of the first frame, as the frame rows show it.
    uint64_t firstNumber = 0;
    const char* name = "";

    // Frames with a known end.
    size_t Complete() const;
    int64_t Start( size_t i ) const { return ( *starts )[i]; }
    int64_t End( size_t i ) const;
};

// Draws a plot lane (FPS, usage, GPU memory) over the view, as the threads
// show it: an opaque background (`background`, which can reach above the area
// to cover the lane's header row), the lane, and a dashed line at the view's
// middle, the current frame. Returns the time under the mouse.
std::optional<int64_t> DrawPlotLane( ImDrawList* draw, const VramPlotArea& area, const VramPlotArea& background,
    const std::function<std::optional<int64_t>( const VramPlotArea& )>& drawLane );

const char* FpsLaneLabel();
float FpsLaneHeight( float plotHeight );
// Draws the FPS lane: per pixel the average FPS over the frames it covers,
// colored against the target, the FPS of its slowest frame as a line, and
// the target. Returns the time under the mouse, after showing its tooltip.
std::optional<int64_t> DrawFpsLane( ImDrawList* draw, const VramPlotArea& area, const FrameTimes& frames, int target );
// The header tooltip of the FPS lane: the capture's average, median and
// 99th percentile frames, and the frames slower than the target.
void FpsLaneTooltip( const FrameTimes& frames, int target );

}

#endif
