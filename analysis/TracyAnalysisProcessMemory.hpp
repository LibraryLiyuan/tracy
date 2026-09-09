#ifndef __TRACYANALYSISPROCESSMEMORY_HPP__
#define __TRACYANALYSISPROCESSMEMORY_HPP__

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace tracy::analysis
{
struct AnalysisProcessMemorySample
{
    bool available=false;
    uint64_t residentBytes=0,privateBytes=0;
};
AnalysisProcessMemorySample ReadAnalysisProcessMemory();
struct AnalysisProcessMemoryOptions
{
    uint64_t maximumBytes=32ull*1024*1024*1024;
    std::chrono::milliseconds interval{100};
    std::function<AnalysisProcessMemorySample()> sample=ReadAnalysisProcessMemory;
};
struct AnalysisProcessMemorySnapshot
{
    uint64_t maximumBytes=0,samples=0,residentBytes=0,privateBytes=0;
    uint64_t peakResidentBytes=0,peakPrivateBytes=0;
    std::string error;
};
// Cooperative, sampled whole-process protection, NOT an allocator or OS cap.
// Windows measures WorkingSetSize and PrivateUsage, including the source Worker.
// Linux private bytes are private resident + swap; macOS uses physical footprint.
// Each execution owns a fresh guard: a failure is latched until that execution
// has stopped. A later resume may succeed once capacity becomes available.
class AnalysisProcessMemoryGuard
{
public:
    explicit AnalysisProcessMemoryGuard(AnalysisProcessMemoryOptions options={});
    // Force at publication/response boundaries; false is a cheap per-record
    // checkpoint that samples at most once per configured interval.
    void Check(bool forceSample=true);
    AnalysisProcessMemorySnapshot Snapshot() const;
    // The guard must outlive the returned monitor. Destroying the monitor joins
    // it and interrupts its timed wait; it never detaches from an execution.
    std::jthread Monitor(std::stop_source stop);
private:
    AnalysisProcessMemoryOptions m_options;
    mutable std::mutex m_mutex;
    AnalysisProcessMemorySnapshot m_snapshot;
    std::chrono::steady_clock::time_point m_nextSample{};
};
}
#endif
