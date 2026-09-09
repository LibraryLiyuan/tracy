#ifndef __TRACYANALYSISSCANMANAGER_HPP__
#define __TRACYANALYSISSCANMANAGER_HPP__

#include "TracyCandidatePolicy.hpp"
#include "TracyDeterministicScanTypes.hpp"
#include "TracyTraceSource.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"
#include "TracyAnalysisProcessMemory.hpp"
#include "TracyAnalysisDiskBudget.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tracy::query
{

struct AnalysisScanProducts
{
    AnalysisScanProducts() = default;
    AnalysisScanProducts(const AnalysisScanProducts&) = delete;
    AnalysisScanProducts& operator=(const AnalysisScanProducts&) = delete;
    AnalysisScanProducts(AnalysisScanProducts&&) noexcept = default;
    AnalysisScanProducts& operator=(AnalysisScanProducts&& other) noexcept
    {
        if(this!=&other)
        {
            AnalysisScanProducts replacement(std::move(other));
            using std::swap;
            swap(metadataWorkspace,replacement.metadataWorkspace);
            swap(contextWorkspace,replacement.contextWorkspace);
            swap(neutralCachePath,replacement.neutralCachePath);
            swap(neutralCacheIdentity,replacement.neutralCacheIdentity);
            swap(aggregate,replacement.aggregate);
            swap(signatureContexts,replacement.signatureContexts);
            swap(capacityFacts,replacement.capacityFacts);
            swap(frameTimelines,replacement.frameTimelines);
            swap(frameSeriesPath,replacement.frameSeriesPath);
        }
        return *this;
    }
    // Declared before the owned vectors: released only after their destruction.
    analysis::AnalysisWorkspaceReservation metadataWorkspace;
    analysis::AnalysisWorkspaceReservation contextWorkspace;
    // The default executor returns a completed, immutable cache and small
    // audit/metadata only. Vectors remain for explicitly injected executors.
    std::filesystem::path neutralCachePath;
    std::string neutralCacheIdentity;
    analysis::NeutralStatisticsResult aggregate;
    std::vector<analysis::PolicySignatureContext> signatureContexts;
    std::vector<analysis::PolicyCapacityFact> capacityFacts;
    std::vector<analysis::PolicyFrameTimeline> frameTimelines;
    std::filesystem::path frameSeriesPath;
};

struct AnalysisScanExecutionRequest
{
    std::string scanId;
    std::string traceSessionId;
    std::filesystem::path tracePath;
    std::shared_ptr<analysis::TraceSource> source;
    nlohmann::json normalizedProfile;
    std::string profileIdentity;
    analysis::NeutralAggregateIdentity aggregateIdentity;
    std::filesystem::path aggregateRoot;
    std::filesystem::path temporaryRoot;
    std::shared_ptr<analysis::AnalysisWorkspaceBudget> workspace;
    std::function<void()> checkDiskSpace;
    std::shared_ptr<analysis::AnalysisDiskBudget> disk;
};

using AnalysisScanProgressCallback = std::function<void(
    analysis::ScanState, uint64_t, uint64_t, std::string_view )>;
using AnalysisScanExecutor = std::function<bool( const AnalysisScanExecutionRequest&,
    std::stop_token, const AnalysisScanProgressCallback&, AnalysisScanProducts&, std::string& )>;
using AnalysisScanSourceResolver = std::function<std::shared_ptr<analysis::TraceSource>(
    const std::filesystem::path&, std::stop_token )>;

bool ExecuteDefaultAnalysisScan( const AnalysisScanExecutionRequest& request,
    std::stop_token stopToken, const AnalysisScanProgressCallback& progress,
    AnalysisScanProducts& products, std::string& error );

inline constexpr size_t AnalysisScanPagePayloadBudgetBytes = 3 * 1024 * 1024;

nlohmann::json PaginateAnalysisScanItems( const nlohmann::json& source, size_t limit,
    const std::string& cursor, const std::vector<std::string>& fields,
    const nlohmann::json& filter );

struct AnalysisScanStartRequest
{
    std::string traceSessionId;
    std::filesystem::path tracePath;
    std::shared_ptr<analysis::TraceSource> source;
    nlohmann::json normalizedProfile;
    std::string profileIdentity;
};

struct AnalysisScanSnapshot
{
    std::string scanId;
    analysis::ScanState state = analysis::ScanState::Failed;
    bool resumable = false;
    bool completed = false;
    bool closed = false;
    uint64_t progressCompleted = 0;
    uint64_t progressTotal = 0;
    std::string stage;
    std::string error;
    analysis::AnalysisProcessMemorySnapshot processMemory;
};

nlohmann::json AnalysisProcessMemorySnapshotJson(const analysis::AnalysisProcessMemorySnapshot& value);

class AnalysisScanManager
{
public:
    AnalysisScanManager( std::filesystem::path root, std::filesystem::path cacheRoot,
        std::string queryExecutableSha256, AnalysisScanSourceResolver resolver,
        AnalysisScanExecutor executor,
        std::shared_ptr<analysis::AnalysisWorkspaceBudget> workspace = {},
        analysis::AnalysisProcessMemoryOptions processMemory = {},
        std::function<uint64_t(const std::filesystem::path&)> diskAvailable = {} );
    ~AnalysisScanManager();

    AnalysisScanManager( const AnalysisScanManager& ) = delete;
    AnalysisScanManager& operator=( const AnalysisScanManager& ) = delete;

    AnalysisScanSnapshot Start( const AnalysisScanStartRequest& request );
    AnalysisScanSnapshot Status( const std::string& scanId ) const;
    AnalysisScanSnapshot Cancel( const std::string& scanId );
    AnalysisScanSnapshot Resume( const std::string& scanId );
    AnalysisScanSnapshot Close( const std::string& scanId );

    nlohmann::json Summary( const std::string& scanId ) const;
    nlohmann::json Signatures( const std::string& scanId, size_t limit,
        const std::string& cursor, const std::vector<std::string>& fields,
        const nlohmann::json& filter ) const;
    nlohmann::json Candidates( const std::string& scanId, size_t limit,
        const std::string& cursor, const std::vector<std::string>& fields,
        const nlohmann::json& filter ) const;
    nlohmann::json Candidate( const std::string& scanId, const std::string& candidateId ) const;
    nlohmann::json RepresentativeFrames( const std::string& scanId,
        const std::string& candidateId ) const;
    nlohmann::json Quality( const std::string& scanId ) const;

private:
    struct Entry;

    std::shared_ptr<Entry> FindOrLoad( const std::string& scanId ) const;
    void StartWorker( const std::shared_ptr<Entry>& entry );
    void Run( const std::shared_ptr<Entry>& entry, std::stop_token stopToken,
        analysis::AnalysisProcessMemoryGuard& memory );
    void Update( const std::shared_ptr<Entry>& entry, analysis::ScanState state,
        uint64_t completed, uint64_t total, std::string_view stage,
        std::string_view error = {} ) const;
    bool SaveStateLocked( const Entry& entry, std::string& error ) const;
    std::shared_ptr<Entry> LoadEntry( const std::string& scanId ) const;
    // Caller holds Entry::readMutex; readers are pinned to completed generations.
    void EnsureReaders( const std::shared_ptr<Entry>& entry ) const;
    std::filesystem::path m_root;
    std::filesystem::path m_cacheRoot;
    std::string m_queryExecutableSha256;
    AnalysisScanSourceResolver m_resolver;
    AnalysisScanExecutor m_executor;
    std::shared_ptr<analysis::AnalysisWorkspaceBudget> m_workspace;
    analysis::AnalysisProcessMemoryOptions m_processMemory;
    std::function<uint64_t(const std::filesystem::path&)> m_diskAvailable;
    std::shared_ptr<analysis::AnalysisDiskUsage> m_diskUsage;
    mutable std::mutex m_mutex;
    mutable std::unordered_map<std::string, std::shared_ptr<Entry>> m_entries;
};

}

#endif
