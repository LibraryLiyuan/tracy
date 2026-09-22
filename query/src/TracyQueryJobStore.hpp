#ifndef __TRACYQUERYJOBSTORE_HPP__
#define __TRACYQUERYJOBSTORE_HPP__

#include "TracyPagedRecordStore.hpp"
#include "TracyTraceSource.hpp"
#include "TracyJnData.hpp"
#include "TracyQueue.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tracy::query
{

enum class QueryJobDomain : uint8_t
{
    Type,
    Schedule,
    Config,
    Dependency,
    Stage,
    GfxDispatch,
    Frame,
    Callsite,
    Count
};

inline constexpr size_t QueryJobDomainCount = size_t( QueryJobDomain::Count );

struct QueryJobBuildStatistics
{
    uint64_t aggregationCompressedFactPageLoads = 0;
    uint64_t aggregationCompressedFactStoredBytes = 0;
    uint64_t aggregationCompressedFactDecodedBytes = 0;
    uint64_t aggregationCompressedFactDecompressions = 0;
    uint64_t aggregationMappedFactReads = 0;
    uint64_t mappedFactBytes = 0;
    uint64_t completionLookupBlockLoads = 0;
    uint64_t completionLookupStoredBytes = 0;
    uint64_t completionLookupDecodedBytes = 0;
    uint64_t completionLookupDecompressions = 0;
    uint64_t jobNameRequests = 0;
    uint64_t jobNameResolutions = 0;
    uint64_t jobNameCacheHits = 0;
};

class QueryJobStoreWriter
{
public:
    QueryJobStoreWriter( std::filesystem::path root, std::string sourceFingerprint,
        analysis::PagedRecordOptions options = {} );
    ~QueryJobStoreWriter();
    QueryJobStoreWriter( const QueryJobStoreWriter& ) = delete;
    QueryJobStoreWriter& operator=( const QueryJobStoreWriter& ) = delete;

    void Begin( QueryJobDomain domain, uint64_t count );
    void Append( QueryJobDomain domain, uint64_t offset, const void* records, uint64_t count );
    void End( QueryJobDomain domain );
    void FinishMissing();
    QueryJobBuildStatistics BuildIndexes( const std::filesystem::path& strings );
    uint64_t Count( QueryJobDomain domain ) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class QueryJobStoreReader
{
public:
    QueryJobStoreReader( std::filesystem::path root, std::filesystem::path strings,
        std::string sourceFingerprint, analysis::PagedRecordOptions options = {} );
    ~QueryJobStoreReader();
    QueryJobStoreReader( const QueryJobStoreReader& ) = delete;
    QueryJobStoreReader& operator=( const QueryJobStoreReader& ) = delete;

    uint64_t GetJobCount() const;
    std::optional<std::string> FindJobRef( uint64_t jobId, const std::function<void()>& check ) const;
    uint64_t Count( QueryJobDomain domain ) const;
    std::vector<analysis::JobDto> GetJobs() const;
    std::vector<analysis::JobDto> ScanJobs( size_t offset, size_t limit ) const;
    std::vector<analysis::JobDto> ScanJobsBySchedule( size_t offset, size_t limit,
        const std::function<void()>& check ) const;
    std::vector<analysis::JobDto> GetDirectedJobs( uint64_t jobId, bool includeNeighbors,
        const std::function<void()>& check ) const;
    std::vector<analysis::JobDto> GetEvidenceJobs( uint64_t frameId,
        const std::function<void()>& check ) const;
    uint64_t GetGfxDispatchCount() const;
    std::vector<analysis::GfxDispatchDto> GetGfxDispatches() const;
    std::vector<analysis::GfxDispatchDto> ScanGfxDispatches( size_t offset, size_t limit ) const;
    std::vector<analysis::CorrelatedFrameEventDto> GetCorrelatedFrameEvents() const;
    uint64_t GetCallsiteCount() const;
    std::vector<analysis::CallsiteDto> GetCallsites() const;
    std::vector<analysis::CallsiteDto> ScanCallsites( size_t offset, size_t limit ) const;
    void ValidateAll();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
