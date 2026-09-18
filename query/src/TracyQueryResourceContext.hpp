#ifndef __TRACYQUERYRESOURCECONTEXT_HPP__
#define __TRACYQUERYRESOURCECONTEXT_HPP__

#include "TracyAnalysisDiskBudget.hpp"
#include "TracyAnalysisProcessMemory.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace tracy::query
{
struct QueryResourceContextOptions
{
    uint64_t traceBytes=0;
    std::vector<std::filesystem::path> derivedRoots;
    uint64_t privateBytesSoftTarget=16ull*1024*1024*1024;
    uint64_t processCommitHardLimit=32ull*1024*1024*1024;
    uint64_t workspaceMaximumBytes=4ull*1024*1024*1024;
    uint64_t workspaceTargetBytes=2ull*1024*1024*1024;
    uint64_t diagnosticDiskReserveBytes=1024*1024;
    std::chrono::milliseconds sampleInterval{100};
    std::function<analysis::AnalysisProcessMemorySample()> memorySample=analysis::ReadAnalysisProcessMemory;
    std::function<void()> relieveMemoryPressure;
};

class QueryResourceError final : public std::runtime_error
{
public:
    QueryResourceError(std::string category,std::string reason,std::optional<uint64_t> requestedBytes={});
    const std::string& Code() const { return m_code; }
    const std::string& Reason() const { return m_reason; }
    std::optional<uint64_t> RequestedBytes() const { return m_requestedBytes; }
private:
    std::string m_code="RESOURCE_LIMIT";
    std::string m_reason;
    std::optional<uint64_t> m_requestedBytes;
};

struct QueryResourceDiskSnapshot
{
    uint64_t currentBytes=0,peakBytes=0,finalLimitBytes=0,peakLimitBytes=0,rejectedGrowths=0;
};

struct QueryResourceSnapshot
{
    std::string phase;
    uint64_t progressCurrent=0,progressTotal=0;
    uint32_t liveWorkers=0;
    analysis::AnalysisProcessMemorySnapshot memory;
    analysis::AnalysisWorkspaceSnapshot workspace;
    QueryResourceDiskSnapshot disk;
    std::string failureReason;
    bool requestedBytesKnown=false;
    uint64_t requestedBytes=0;
};

class QueryResourceContext
{
public:
    explicit QueryResourceContext(QueryResourceContextOptions options);
    ~QueryResourceContext();
    QueryResourceContext(const QueryResourceContext&)=delete;
    QueryResourceContext& operator=(const QueryResourceContext&)=delete;

    std::shared_ptr<analysis::AnalysisWorkspaceBudget> Workspace() const { return m_workspace; }
    std::shared_ptr<analysis::AnalysisDiskBudget> Disk() const { return m_disk; }
    void SetPhase(std::string phase);
    void SetProgress(uint64_t current,uint64_t total);
    void SetLiveWorkers(uint32_t workers);
    void CheckMemory(bool forceSample=true);
    void CheckFinalDisk();
    void RunResourceBoundary(const std::function<void()>& operation);
    QueryResourceSnapshot Snapshot() const;

private:
    struct State;
    void RecordFailure(std::string reason,std::optional<uint64_t> requestedBytes={}) const;
    void ObserveSoftTarget();

    QueryResourceContextOptions m_options;
    analysis::AnalysisDiskBudget::TraceLimits m_limits;
    std::shared_ptr<State> m_state;
    std::shared_ptr<analysis::AnalysisProcessMemoryGuard> m_memory;
    std::shared_ptr<analysis::AnalysisWorkspaceBudget> m_workspace;
    std::shared_ptr<analysis::AnalysisDiskUsage> m_usage;
    std::shared_ptr<analysis::AnalysisDiskBudget> m_disk;
    std::unique_ptr<analysis::AnalysisDiskActivity> m_diskActivity;
};
}

#endif
