#ifndef __TRACYANALYSISSCANMANAGER_HPP__
#define __TRACYANALYSISSCANMANAGER_HPP__

#include "TracyCandidatePolicy.hpp"
#include "TracyDeterministicScanTypes.hpp"
#include "TracyTraceSource.hpp"

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
};

class AnalysisScanManager
{
public:
    AnalysisScanManager( std::filesystem::path root, std::filesystem::path cacheRoot,
        std::string queryExecutableSha256, AnalysisScanSourceResolver resolver,
        AnalysisScanExecutor executor );
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
    void Run( const std::shared_ptr<Entry>& entry, std::stop_token stopToken );
    void Update( const std::shared_ptr<Entry>& entry, analysis::ScanState state,
        uint64_t completed, uint64_t total, std::string_view stage,
        std::string_view error = {} ) const;
    bool SaveStateLocked( const Entry& entry, std::string& error ) const;
    std::shared_ptr<Entry> LoadEntry( const std::string& scanId ) const;
    nlohmann::json ReadAggregateDocument( const Entry& entry ) const;
    nlohmann::json ReadCandidateDocument( const Entry& entry ) const;
    std::filesystem::path m_root;
    std::filesystem::path m_cacheRoot;
    std::string m_queryExecutableSha256;
    AnalysisScanSourceResolver m_resolver;
    AnalysisScanExecutor m_executor;
    mutable std::mutex m_mutex;
    mutable std::unordered_map<std::string, std::shared_ptr<Entry>> m_entries;
};

}

#endif
