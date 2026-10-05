#ifndef __TRACYFRAMETRIAGESOURCE_HPP__
#define __TRACYFRAMETRIAGESOURCE_HPP__

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace tracy::analysis
{

// Optional fast paths of a trace source for the frame triage. Without them
// the triage pages through ScanGpuZones and ScanPlots.
class FrameTriageSource
{
public:
    virtual ~FrameTriageSource() = default;
    // Each GPU zone without a parent that has a GPU start and end, as
    // (GpuContextDto::index, start, end).
    virtual void VisitTopLevelGpuZones( const std::function<void( size_t, int64_t, int64_t )>& visit ) const = 0;
    // The points of the plot with this PlotDto::ref, in time order.
    virtual void VisitPlotPoints( std::string_view plotRef, const std::function<void( int64_t, double )>& visit ) const = 0;
};

}

#endif
