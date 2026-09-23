#include "TracyQueryResourceContext.hpp"
#include "TracyFileReadHooks.hpp"
#include "TracyFileWriteHooks.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

namespace tracy::query
{
namespace
{
std::string ErrorMessage(const std::string& category,const std::string& reason)
{
    return "query_resource_"+category+":"+reason;
}
analysis::AnalysisDiskBudget::TraceLimits ProfileDiskLimits(const QueryResourceContextOptions& options)
{
    const auto defaults=analysis::AnalysisDiskBudget::ForTrace(options.traceBytes);
    if(!options.expectedFinalDiskLimit && !options.expectedPeakDiskLimit) return defaults;
    if(!options.expectedFinalDiskLimit || !options.expectedPeakDiskLimit ||
        options.expectedFinalDiskLimit>options.expectedPeakDiskLimit ||
        options.expectedFinalDiskLimit>defaults.finalBytes ||
        options.expectedPeakDiskLimit>defaults.peakBytes)
        throw std::runtime_error("query_resource_configuration");
    return {options.expectedFinalDiskLimit,options.expectedPeakDiskLimit};
}
}

struct QueryResourceContext::State
{
    mutable std::mutex mutex;
    std::mutex fileMutex;
    std::mutex temporaryMutex;
    std::filesystem::path statisticsTemporaryRoot;
    std::string phase;
    std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now(),nextProgressLog=started;
    uint64_t phaseStartedMs=0;
    uint64_t progressCurrent=0,progressTotal=0;
    uint32_t liveWorkers=0;
    std::string failureReason;
    std::optional<uint64_t> requestedBytes;
    bool softPressureActive=false;
    std::unordered_map<std::string,uint64_t> fileBytes;
    std::unordered_map<std::string,std::pair<uint64_t,uint64_t>> containers;
    std::vector<QueryResourcePhaseSnapshot> phaseHistory;
    bool phaseHistoryTruncated=false;
};

QueryResourceError::QueryResourceError(std::string category,std::string reason,
    std::optional<uint64_t> requestedBytes)
    :std::runtime_error(ErrorMessage(category,reason)),m_reason(std::move(reason)),m_requestedBytes(requestedBytes)
{}

QueryResourceContext::QueryResourceContext(QueryResourceContextOptions options)
    :m_options(std::move(options)),m_limits(ProfileDiskLimits(m_options)),
    m_state(std::make_shared<State>())
{
    if(m_options.derivedRoots.empty() || !m_options.privateBytesSoftTarget ||
        m_options.privateBytesSoftTarget>m_options.processCommitHardLimit || !m_options.memorySample ||
        (!m_options.expectedTraceSha256.empty() && m_options.expectedTraceSha256.size()!=64))
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
    m_diagnosticWorkspace=analysis::AnalysisWorkspaceReservation(m_workspace,m_options.diagnosticMemoryReserveBytes);

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
    try { m_disk->Grow(0); }
    catch(const std::exception&)
    {
        RecordFailure("disk_peak_limit");
        throw QueryResourceError("disk","disk_peak_limit");
    }
}

QueryResourceContext::~QueryResourceContext()
{
    if(!m_state->statisticsTemporaryRoot.empty())
    {
        std::error_code error;
        analysis::AnalysisDiskRemove(m_state->statisticsTemporaryRoot,m_disk,error,true);
    }
}

std::filesystem::path QueryResourceContext::StatisticsTemporaryRoot()
{
    std::lock_guard lock(m_state->temporaryMutex);
    if(!m_state->statisticsTemporaryRoot.empty()) return m_state->statisticsTemporaryRoot;
    const auto parent=std::filesystem::temp_directory_path()/"tracy-query-statistics";
    std::filesystem::create_directories(parent);
    static std::atomic<uint64_t> sequence{0};
    for(size_t attempt=0;attempt<100;attempt++)
    {
        const auto path=parent/("owner-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
            "-"+std::to_string(sequence.fetch_add(1)));
        if(!std::filesystem::create_directory(path)) continue;
        try
        {
            IncludeDiskRoots({path});
            m_state->statisticsTemporaryRoot=path;
        }
        catch(...)
        {
            std::error_code error;
            std::filesystem::remove(path,error);
            throw;
        }
        return path;
    }
    throw std::runtime_error("query_statistics_temporary_collision");
}

void QueryResourceContext::SetPhase(std::string phase)
{
    {
        std::lock_guard lock(m_state->mutex);
        if(m_state->phase==phase)return;
    }
    const auto disk=m_usage->Snapshot();
    const auto workspace=m_workspace->Snapshot();
    const auto memory=m_memory->Snapshot();
    std::optional<QueryResourcePhaseSnapshot> ended;
    uint64_t elapsed=0;
    {
        std::lock_guard lock(m_state->mutex);
        if(m_state->phase==phase)return;
        elapsed=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-m_state->started).count());
        if(!m_state->phase.empty())
        {
            QueryResourcePhaseSnapshot value;
            value.phase=m_state->phase;
            value.startMs=m_state->phaseStartedMs;
            value.endMs=elapsed;
            value.durationMs=elapsed-value.startMs;
            value.progressCurrent=m_state->progressCurrent;
            value.progressTotal=m_state->progressTotal;
            value.diskCurrentBytes=disk.currentBytes;
            value.diskPeakBytes=disk.peakBytes;
            value.workspaceCurrentBytes=workspace.currentBytes;
            value.workspacePeakBytes=workspace.peakBytes;
            value.privateBytes=memory.privateBytes;
            value.peakPrivateBytes=memory.peakPrivateBytes;
            value.complete=true;
            ended=value;
            if(m_state->phaseHistory.size()<256) m_state->phaseHistory.emplace_back(std::move(value));
            else m_state->phaseHistoryTruncated=true;
        }
        m_state->phase=phase;
        m_state->phaseStartedMs=elapsed;
        m_state->progressCurrent=0;
        m_state->progressTotal=0;
        m_state->nextProgressLog=std::chrono::steady_clock::now();
    }
    if(ended)
        std::fprintf(stderr,"Query phase-end=%.*s duration_ms=%llu disk_current_bytes=%llu disk_peak_bytes=%llu\n",
            128,ended->phase.c_str(),(unsigned long long)ended->durationMs,
            (unsigned long long)ended->diskCurrentBytes,(unsigned long long)ended->diskPeakBytes);
    std::fprintf(stderr,"Query phase=%.*s elapsed_ms=%llu\n",128,phase.c_str(),(unsigned long long)elapsed);
}

void QueryResourceContext::SetProgress(uint64_t current,uint64_t total)
{
    std::string phase;int64_t elapsed=0;bool emit=false;
    {
        std::lock_guard lock(m_state->mutex);
        m_state->progressCurrent=current;m_state->progressTotal=total;
        const auto now=std::chrono::steady_clock::now();
        if(now>=m_state->nextProgressLog) {
            m_state->nextProgressLog=now+std::chrono::seconds(5);
            phase=m_state->phase.substr(0,128);emit=true;
            elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(now-m_state->started).count();
        }
    }
    if(emit)std::fprintf(stderr,"Query progress phase=%s current=%llu total=%llu elapsed_ms=%lld\n",phase.c_str(),
        (unsigned long long)current,(unsigned long long)total,(long long)elapsed);
}

void QueryResourceContext::SetLiveWorkers(uint32_t workers)
{
    std::lock_guard lock(m_state->mutex);
    m_state->liveWorkers=workers;
}

void QueryResourceContext::SetContainer(std::string name,uint64_t size,uint64_t capacity)
{
    std::lock_guard lock(m_state->mutex);
    m_state->containers[std::move(name)]={size,capacity};
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

void QueryResourceContext::ValidateTraceIdentity(uint64_t bytes,std::string_view sha256) const
{
    if((m_options.expectedTraceBytes && m_options.expectedTraceBytes!=bytes) ||
        (m_options.expectedFinalDiskLimit && m_options.expectedFinalDiskLimit!=m_limits.finalBytes) ||
        (m_options.expectedPeakDiskLimit && m_options.expectedPeakDiskLimit!=m_limits.peakBytes) ||
        (!m_options.expectedTraceSha256.empty() && m_options.expectedTraceSha256!=sha256))
        throw std::runtime_error("query_resource_profile_identity_mismatch");
}

void QueryResourceContext::BeforeFileGrowth(const std::filesystem::path& path,uint64_t proposedBytes)
{
    const auto key=std::filesystem::absolute(path).lexically_normal().generic_string();
    std::lock_guard fileLock(m_state->fileMutex);
    auto& charged=m_state->fileBytes[key];
    if(proposedBytes<=charged) return;
    const auto growth=proposedBytes-charged;
    try { m_disk->Grow(growth); }
    catch(const std::exception&)
    {
        RecordFailure("disk_peak_limit",growth);
        throw QueryResourceError("disk","disk_peak_limit",growth);
    }
    charged=proposedBytes;
}

std::shared_ptr<tracy::FileWriteHooks> QueryResourceContext::CreateFileWriteHooks(const std::filesystem::path& path)
{
    auto hooks=std::make_shared<tracy::FileWriteHooks>();
    auto workspace=std::make_shared<analysis::AnalysisWorkspaceReservation>(m_workspace);
    hooks->check=[this] { CheckMemory(false); };
    hooks->beforeWrite=[this,path](uint64_t bytes) { BeforeFileGrowth(path,bytes); };
    hooks->reserveMemory=[workspace](uint64_t bytes) { workspace->Resize(bytes); };
    return hooks;
}

std::shared_ptr<tracy::FileReadHooks> QueryResourceContext::CreateFileReadHooks()
{
    auto hooks=std::make_shared<tracy::FileReadHooks>();
    auto workspace=std::make_shared<analysis::AnalysisWorkspaceReservation>(m_workspace);
    hooks->check=[this] { CheckMemory(false); };
    hooks->reserveMemory=[workspace](uint64_t bytes) { workspace->Resize(bytes); };
    return hooks;
}

void QueryResourceContext::IncludeDiskRoots(const std::vector<std::filesystem::path>& roots)
{
    try { m_usage->AddRoots(roots,m_limits.peakBytes); }
    catch(const std::exception&)
    {
        RecordFailure("disk_existing_limit");
        throw QueryResourceError("disk","disk_existing_limit");
    }
}

void QueryResourceContext::WriteDiagnostic(const std::filesystem::path& path)
{
    const auto snapshot=Snapshot();
    nlohmann::json containers=nlohmann::json::array();
    for(const auto& value:snapshot.containers) containers.push_back({
        {"name",value.name},{"size",std::to_string(value.size)},{"capacity",std::to_string(value.capacity)}});
    nlohmann::json phases=nlohmann::json::array();
    for(const auto& value:snapshot.phaseHistory) phases.push_back({
        {"phase",value.phase},{"start_ms",value.startMs},{"end_ms",value.endMs},
        {"duration_ms",value.durationMs},{"progress_current",std::to_string(value.progressCurrent)},
        {"progress_total",std::to_string(value.progressTotal)},
        {"disk_current_bytes",std::to_string(value.diskCurrentBytes)},
        {"disk_peak_bytes",std::to_string(value.diskPeakBytes)},
        {"workspace_current_bytes",std::to_string(value.workspaceCurrentBytes)},
        {"workspace_peak_bytes",std::to_string(value.workspacePeakBytes)},
        {"private_bytes",std::to_string(value.privateBytes)},
        {"peak_private_bytes",std::to_string(value.peakPrivateBytes)},
        {"complete",value.complete}
    });
    nlohmann::json report={
        {"schema",1},
        {"phase",snapshot.phase},
        {"progress_current",std::to_string(snapshot.progressCurrent)},
        {"progress_total",std::to_string(snapshot.progressTotal)},
        {"live_workers",snapshot.liveWorkers},
        {"failure_reason",snapshot.failureReason},
        {"requested_bytes",snapshot.requestedBytesKnown?nlohmann::json(std::to_string(snapshot.requestedBytes)):nlohmann::json(nullptr)},
        {"containers",std::move(containers)},
        {"phase_history",std::move(phases)},
        {"phase_history_truncated",snapshot.phaseHistoryTruncated},
        {"memory",{
            {"private_bytes",std::to_string(snapshot.memory.privateBytes)},
            {"peak_private_bytes",std::to_string(snapshot.memory.peakPrivateBytes)},
            {"working_set_bytes",std::to_string(snapshot.memory.residentBytes)},
            {"peak_working_set_bytes",std::to_string(snapshot.memory.peakResidentBytes)},
            {"system_commit_available",snapshot.memory.systemCommitAvailable},
            {"system_commit_limit_bytes",std::to_string(snapshot.memory.systemCommitLimitBytes)},
            {"system_commit_bytes",std::to_string(snapshot.memory.systemCommitBytes)},
            {"system_commit_headroom_bytes",std::to_string(snapshot.memory.systemCommitHeadroomBytes)},
            {"minimum_system_commit_headroom_bytes",std::to_string(snapshot.memory.minimumSystemCommitHeadroomBytes)}
        }},
        {"workspace",{
            {"current_bytes",std::to_string(snapshot.workspace.currentBytes)},
            {"peak_bytes",std::to_string(snapshot.workspace.peakBytes)},
            {"maximum_bytes",std::to_string(snapshot.workspace.maximumBytes)},
            {"rejected_reservations",std::to_string(snapshot.workspace.rejectedReservations)}
        }},
        {"disk",{
            {"current_bytes",std::to_string(snapshot.disk.currentBytes)},
            {"peak_bytes",std::to_string(snapshot.disk.peakBytes)},
            {"final_limit_bytes",std::to_string(snapshot.disk.finalLimitBytes)},
            {"peak_limit_bytes",std::to_string(snapshot.disk.peakLimitBytes)},
            {"rejected_growths",std::to_string(snapshot.disk.rejectedGrowths)}
        }}
    };
    const auto payload=report.dump(2);
    if(!m_options.diagnosticDiskReserveBytes || payload.size()>m_options.diagnosticDiskReserveBytes)
        throw std::runtime_error("query_resource_diagnostic_reserve");

    const auto key=std::filesystem::absolute(path).lexically_normal().generic_string();
    {
        std::lock_guard fileLock(m_state->fileMutex);
        auto& charged=m_state->fileBytes[key];
        if(payload.size()>charged)
        {
            const auto growth=uint64_t(payload.size())-charged;
            if(m_disk->checkSpace) m_disk->checkSpace();
            m_usage->Grow(growth,m_limits.peakBytes,false);
            charged=payload.size();
        }
    }
    std::ofstream output(path,std::ios::binary|std::ios::trunc);
    if(!output) throw std::runtime_error("query_resource_diagnostic_open");
    output.write(payload.data(),std::streamsize(payload.size()));
    output.flush();
    if(!output) throw std::runtime_error("query_resource_diagnostic_write");
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
        if(message.starts_with("query_resource_memory:"))
        {
            RecordFailure("process_private_hard_limit");
            throw QueryResourceError("memory","process_private_hard_limit");
        }
        if(message.starts_with("query_resource_disk:"))
        {
            auto snapshot=Snapshot();
            if(snapshot.failureReason.empty()) { RecordFailure("disk_limit"); snapshot=Snapshot(); }
            throw QueryResourceError("disk",snapshot.failureReason,
                snapshot.requestedBytesKnown?std::optional<uint64_t>(snapshot.requestedBytes):std::nullopt);
        }
        if(message.starts_with("query_resource_workspace:"))
        {
            auto snapshot=Snapshot();
            if(snapshot.failureReason.empty()) { RecordFailure("workspace_limit"); snapshot=Snapshot(); }
            throw QueryResourceError("workspace",snapshot.failureReason,
                snapshot.requestedBytesKnown?std::optional<uint64_t>(snapshot.requestedBytes):std::nullopt);
        }
        if(message.find("analysis_workspace_budget")!=std::string::npos)
        {
            auto snapshot=Snapshot();
            if(snapshot.failureReason.empty()) { RecordFailure("workspace_limit"); snapshot=Snapshot(); }
            throw QueryResourceError("workspace",snapshot.failureReason,
                snapshot.requestedBytesKnown?std::optional<uint64_t>(snapshot.requestedBytes):std::nullopt);
        }
        if(message.find("disk_budget")!=std::string::npos || message.find("final_disk_ratio")!=std::string::npos)
        {
            auto snapshot=Snapshot();
            if(snapshot.failureReason.empty()) { RecordFailure("disk_limit"); snapshot=Snapshot(); }
            throw QueryResourceError("disk",snapshot.failureReason,
                snapshot.requestedBytesKnown?std::optional<uint64_t>(snapshot.requestedBytes):std::nullopt);
        }
        if(message.find("process_memory_budget")!=std::string::npos)
        {
            RecordFailure("process_private_hard_limit");
            throw QueryResourceError("memory","process_private_hard_limit");
        }
        if(message.find("not enough memory to load trace")!=std::string::npos)
        {
            RecordFailure("allocation_failed");
            throw QueryResourceError("allocation","allocation_failed");
        }
        throw;
    }
}

QueryResourceSnapshot QueryResourceContext::Snapshot() const
{
    QueryResourceSnapshot result;
    uint64_t phaseStartedMs=0;
    {
        std::lock_guard lock(m_state->mutex);
        result.phase=m_state->phase;
        result.progressCurrent=m_state->progressCurrent;
        result.progressTotal=m_state->progressTotal;
        result.liveWorkers=m_state->liveWorkers;
        result.failureReason=m_state->failureReason;
        result.requestedBytesKnown=m_state->requestedBytes.has_value();
        result.requestedBytes=m_state->requestedBytes.value_or(0);
        result.phaseHistory=m_state->phaseHistory;
        result.phaseHistoryTruncated=m_state->phaseHistoryTruncated;
        phaseStartedMs=m_state->phaseStartedMs;
        for(const auto& [name,value]:m_state->containers)
            result.containers.push_back({name,value.first,value.second});
    }
    std::sort(result.containers.begin(),result.containers.end(),[](const auto& lhs,const auto& rhs) { return lhs.name<rhs.name; });
    result.memory=m_memory->Snapshot();
    result.workspace=m_workspace->Snapshot();
    const auto disk=m_usage->Snapshot();
    result.disk={disk.currentBytes,disk.peakBytes,m_limits.finalBytes,m_limits.peakBytes,disk.rejectedGrowths};
    if(!result.phase.empty())
    {
        QueryResourcePhaseSnapshot value;
        value.phase=result.phase;
        value.startMs=phaseStartedMs;
        value.endMs=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-m_state->started).count());
        value.durationMs=value.endMs-value.startMs;
        value.progressCurrent=result.progressCurrent;
        value.progressTotal=result.progressTotal;
        value.diskCurrentBytes=disk.currentBytes;
        value.diskPeakBytes=disk.peakBytes;
        value.workspaceCurrentBytes=result.workspace.currentBytes;
        value.workspacePeakBytes=result.workspace.peakBytes;
        value.privateBytes=result.memory.privateBytes;
        value.peakPrivateBytes=result.memory.peakPrivateBytes;
        result.phaseHistory.emplace_back(std::move(value));
    }
    return result;
}
}
