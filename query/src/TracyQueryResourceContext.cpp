#include "TracyQueryResourceContext.hpp"

#include <mutex>
#include <new>
#include <utility>

namespace tracy::query
{
namespace
{
std::string ErrorMessage(const std::string& category,const std::string& reason)
{
    return "query_resource_"+category+":"+reason;
}
}

struct QueryResourceContext::State
{
    mutable std::mutex mutex;
    std::string phase;
    uint64_t progressCurrent=0,progressTotal=0;
    uint32_t liveWorkers=0;
    std::string failureReason;
    std::optional<uint64_t> requestedBytes;
    bool softPressureActive=false;
};

QueryResourceError::QueryResourceError(std::string category,std::string reason,
    std::optional<uint64_t> requestedBytes)
    :std::runtime_error(ErrorMessage(category,reason)),m_reason(std::move(reason)),m_requestedBytes(requestedBytes)
{}

QueryResourceContext::QueryResourceContext(QueryResourceContextOptions options)
    :m_options(std::move(options)),m_limits(analysis::AnalysisDiskBudget::ForTrace(m_options.traceBytes)),
    m_state(std::make_shared<State>())
{
    if(m_options.derivedRoots.empty() || !m_options.privateBytesSoftTarget ||
        m_options.privateBytesSoftTarget>m_options.processCommitHardLimit || !m_options.memorySample)
        throw std::runtime_error("query_resource_configuration");

    analysis::AnalysisProcessMemoryOptions memoryOptions;
    memoryOptions.maximumBytes=m_options.processCommitHardLimit;
    memoryOptions.interval=m_options.sampleInterval;
    memoryOptions.sample=m_options.memorySample;
    m_memory=std::make_shared<analysis::AnalysisProcessMemoryGuard>(std::move(memoryOptions));

    const std::weak_ptr<State> state=m_state;
    const std::weak_ptr<analysis::AnalysisProcessMemoryGuard> memory=m_memory;
    m_workspace=std::make_shared<analysis::AnalysisWorkspaceBudget>(m_options.workspaceMaximumBytes,
        m_options.workspaceTargetBytes,
        [state,memory](uint64_t requested) {
            if(const auto guard=memory.lock())
            {
                try { guard->Check(false); }
                catch(...)
                {
                    if(const auto value=state.lock())
                    {
                        std::lock_guard lock(value->mutex);
                        value->failureReason="process_private_hard_limit";
                        value->requestedBytes=requested;
                    }
                    throw;
                }
            }
        },
        [state](uint64_t requested) {
            if(const auto value=state.lock())
            {
                std::lock_guard lock(value->mutex);
                value->failureReason="workspace_limit";
                value->requestedBytes=requested;
            }
        });

    m_usage=std::make_shared<analysis::AnalysisDiskUsage>(m_options.derivedRoots);
    m_diskActivity=std::make_unique<analysis::AnalysisDiskActivity>(m_usage,m_options.diagnosticDiskReserveBytes);
    m_disk=std::make_shared<analysis::AnalysisDiskBudget>();
    m_disk->usage=m_usage;
    m_disk->maximumBytes=m_limits.peakBytes;
    m_disk->onReject=[state](uint64_t requested) {
        if(const auto value=state.lock())
        {
            std::lock_guard lock(value->mutex);
            value->failureReason="disk_peak_limit";
            value->requestedBytes=requested;
        }
    };
}

QueryResourceContext::~QueryResourceContext()=default;

void QueryResourceContext::SetPhase(std::string phase)
{
    std::lock_guard lock(m_state->mutex);
    m_state->phase=std::move(phase);
}

void QueryResourceContext::SetProgress(uint64_t current,uint64_t total)
{
    std::lock_guard lock(m_state->mutex);
    m_state->progressCurrent=current;
    m_state->progressTotal=total;
}

void QueryResourceContext::SetLiveWorkers(uint32_t workers)
{
    std::lock_guard lock(m_state->mutex);
    m_state->liveWorkers=workers;
}

void QueryResourceContext::RecordFailure(std::string reason,std::optional<uint64_t> requestedBytes) const
{
    std::lock_guard lock(m_state->mutex);
    m_state->failureReason=std::move(reason);
    m_state->requestedBytes=requestedBytes;
}

void QueryResourceContext::ObserveSoftTarget()
{
    const auto memory=m_memory->Snapshot();
    bool relieve=false;
    {
        std::lock_guard lock(m_state->mutex);
        if(memory.privateBytes>m_options.privateBytesSoftTarget)
        {
            relieve=!m_state->softPressureActive;
            m_state->softPressureActive=true;
        }
        else m_state->softPressureActive=false;
    }
    if(relieve && m_options.relieveMemoryPressure) m_options.relieveMemoryPressure();
}

void QueryResourceContext::CheckMemory(bool forceSample)
{
    try { m_memory->Check(forceSample); }
    catch(const std::exception&)
    {
        RecordFailure("process_private_hard_limit");
        throw QueryResourceError("memory","process_private_hard_limit");
    }
    ObserveSoftTarget();
}

void QueryResourceContext::CheckFinalDisk()
{
    try { m_limits.CheckFinal(m_usage->Current()); }
    catch(const std::exception&)
    {
        RecordFailure("disk_final_limit");
        throw QueryResourceError("disk","analysis_final_disk_ratio");
    }
}

void QueryResourceContext::RunResourceBoundary(const std::function<void()>& operation)
{
    try { operation(); }
    catch(const QueryResourceError&) { throw; }
    catch(const std::bad_alloc&)
    {
        RecordFailure("allocation_failed");
        throw QueryResourceError("allocation","allocation_failed");
    }
    catch(const std::exception& error)
    {
        const std::string message=error.what();
        if(message.find("analysis_workspace_budget")!=std::string::npos)
        {
            RecordFailure("workspace_limit");
            throw QueryResourceError("workspace","workspace_limit");
        }
        if(message.find("disk_budget")!=std::string::npos || message.find("final_disk_ratio")!=std::string::npos)
        {
            RecordFailure("disk_limit");
            throw QueryResourceError("disk","disk_limit");
        }
        if(message.find("process_memory_budget")!=std::string::npos)
        {
            RecordFailure("process_private_hard_limit");
            throw QueryResourceError("memory","process_private_hard_limit");
        }
        throw;
    }
}

QueryResourceSnapshot QueryResourceContext::Snapshot() const
{
    QueryResourceSnapshot result;
    {
        std::lock_guard lock(m_state->mutex);
        result.phase=m_state->phase;
        result.progressCurrent=m_state->progressCurrent;
        result.progressTotal=m_state->progressTotal;
        result.liveWorkers=m_state->liveWorkers;
        result.failureReason=m_state->failureReason;
        result.requestedBytesKnown=m_state->requestedBytes.has_value();
        result.requestedBytes=m_state->requestedBytes.value_or(0);
    }
    result.memory=m_memory->Snapshot();
    result.workspace=m_workspace->Snapshot();
    const auto disk=m_usage->Snapshot();
    result.disk={disk.currentBytes,disk.peakBytes,m_limits.finalBytes,m_limits.peakBytes,disk.rejectedGrowths};
    return result;
}
}
