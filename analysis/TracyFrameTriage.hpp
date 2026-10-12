#ifndef __TRACYFRAMETRIAGE_HPP__
#define __TRACYFRAMETRIAGE_HPP__

#include "TracyFrameTriageSource.hpp"
#include "TracyTraceSource.hpp"

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace tracy::analysis
{

struct FrameTriageOptions
{
    std::function<bool()> cancelled;
    double frameBudgetMs = 1000.0 / 60.0;
    // Resource budgets of the analysis profile, in bytes.
    std::optional<double> gpuMemoryBudgetBytes;
    std::optional<double> cpuMemoryBudgetBytes;
    // A FrameSetDto ref or name; empty selects "Player.Frame", else the first set.
    std::string frameSet;
    // Threads reading CPU zones.
    size_t workers = 4;
    // Timeline segment length; 0 picks at least 10 s and at most 60 segments.
    double segmentSeconds = 0;
    double windowSeconds = 5;
    size_t maxWindows = 6;
    // Optional bounded streaming export; one native fact row per complete
    // frame, without retaining an additional JSON array in memory.
    std::function<void( const nlohmann::json& )> frameSink;
    // Full candidate ledger; independent of representative/window limits.
    std::function<void( const nlohmann::json& )> candidateSink;
    std::function<void( const nlohmann::json& )> nativeSink;
    int64_t memorySampleMaxAgeNs = 1000000000;
    size_t maxRepresentatives = 24;
};

const char* FrameTriageAlgorithmId();

// Labels every complete frame of one frame set with what limited it (main,
// render or job threads, GPU, GPU memory, loading and I/O, pacing) from the
// time those threads spent working and waiting in the frame, the GPU's busy
// time and memory and I/O counters, and returns a bounded summary: limiter
// shares, a timeline of segments, spike clusters and recommended windows for
// a deterministic scan restricted to them.
nlohmann::json RunFrameTriage( const TraceSource& source, const FrameTriageOptions& options );

}

#endif
