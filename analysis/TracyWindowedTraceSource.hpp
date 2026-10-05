#ifndef __TRACYWINDOWEDTRACESOURCE_HPP__
#define __TRACYWINDOWEDTRACESOURCE_HPP__

#include "TracyTraceSource.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tracy::analysis
{

// A closed time interval [startNs, endNs] of a capture.
struct AnalysisTimeWindow
{
    int64_t startNs = 0;
    int64_t endNs = 0;
};

inline constexpr size_t MaximumAnalysisWindows = 64;

// Sorted, with overlapping or touching windows merged. Throws
// std::invalid_argument for an empty list, more than MaximumAnalysisWindows
// windows or a window that ends before it starts.
std::vector<AnalysisTimeWindow> NormalizeAnalysisWindows( std::vector<AnalysisTimeWindow> windows );

// Canonical text of normalized windows, for identities.
std::string AnalysisWindowsText( const std::vector<AnalysisTimeWindow>& windows );

// Whether [start, end] meets one of normalized windows.
bool AnalysisWindowsMeet( const std::vector<AnalysisTimeWindow>& windows, int64_t start, int64_t end );

// Optional fast path of a source for a windowed view: Jobs and GPU zones read
// through time indexes, so the cost follows the windows. Each is read by
// ordinal within a subsequence of ScanJobs or ScanGpuZones, in that order,
// that holds every Job whose schedule-to-completion span, or every GPU zone
// whose GPU interval, meets a window.
class WindowedDomainSource
{
public:
    virtual ~WindowedDomainSource() = default;
    virtual std::vector<JobDto> ScanJobsNear( const std::vector<AnalysisTimeWindow>& windows, size_t offset, size_t limit ) const = 0;
    virtual std::vector<GpuZoneDto> ScanGpuZonesNear( const std::vector<AnalysisTimeWindow>& windows, size_t offset, size_t limit ) const = 0;
};

// Optional interface of a source whose scans are restricted to windows.
class AnalysisWindowSource
{
public:
    virtual ~AnalysisWindowSource() = default;
    virtual const std::vector<AnalysisTimeWindow>& AnalysisWindows() const = 0;
};

// A view of `inner` for a deterministic scan of normalized windows only: the
// frames inside a window (and, for a continuous frame set, the frame after the
// last one, which closes it), and the events of the other scanned domains that
// overlap a window. GPU work, Jobs, relations and graphics records may finish
// after their frame, so they are kept up to a margin after each window.
// Definitions (threads, sources, contexts, script frames, callsites, the GPU
// catalog) and lookups by ref are not restricted.
std::shared_ptr<TraceSource> MakeWindowedTraceSource( std::shared_ptr<TraceSource> inner,
    std::vector<AnalysisTimeWindow> windows, int64_t lateMarginNs = 250'000'000 );

}

#endif
