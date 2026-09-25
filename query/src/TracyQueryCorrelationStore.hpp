#ifndef __TRACYQUERYCORRELATIONSTORE_HPP__
#define __TRACYQUERYCORRELATIONSTORE_HPP__

#include "TracyPagedRecordStore.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tracy::query
{

enum class QueryCorrelationEndpointTag : uint8_t
{
    LinkSource,
    LinkTarget,
    EntitySelf,
    EntityParent,
    DispatchSelf,
    DispatchFrame,
    FrameEvent,
    Count
};

struct QueryCorrelationCandidate
{
    QueryCorrelationEndpointTag tag = QueryCorrelationEndpointTag::LinkSource;
    uint64_t ordinal = 0;
};

struct QueryCorrelationCandidatePage
{
    std::vector<QueryCorrelationCandidate> values;
    uint64_t total = 0;
    bool hasMore = false;
};

struct QueryCorrelationBuildStatistics
{
    uint64_t records = 0;
    uint64_t bucketCount = 0;
    uint64_t partitionCount = 0;
    uint64_t temporaryBytes = 0;
    uint64_t peakPartitionRecords = 0;
    std::array<uint64_t, size_t( QueryCorrelationEndpointTag::Count )> tagCounts {};
};

struct QueryCorrelationEdge
{
    std::string sourceRef;
    std::string targetRef;
    std::string relation;
    uint64_t originFrameId = 0;
    uint8_t orderDomain = 0;
    uint64_t ordinal = 0;
};

struct QueryCorrelationNeighborhood
{
    bool known = false;
    bool edgeLimitExceeded = false;
    std::vector<QueryCorrelationEdge> edges;
};

struct QueryCorrelationReadControl
{
    std::function<void()> check;
    std::function<size_t( size_t )> allowance;
    std::function<void( size_t, size_t, size_t )> scanned;
    size_t maximumEdges = 200000;
};

class QueryCorrelationSource
{
public:
    virtual ~QueryCorrelationSource() = default;
    virtual QueryCorrelationNeighborhood GetIndexedCorrelationNeighbors(
        const std::string& ref, const QueryCorrelationReadControl& control ) const = 0;
};

uint64_t QueryCorrelationBucketForTesting( uint64_t nodeId, uint8_t bucketBits );

class QueryCorrelationStoreWriter
{
public:
    QueryCorrelationStoreWriter( std::filesystem::path root, std::string sourceFingerprint,
        uint64_t expectedRecords, analysis::PagedRecordOptions options = {}, uint8_t bucketBitsOverride = 0 );
    ~QueryCorrelationStoreWriter();
    QueryCorrelationStoreWriter( const QueryCorrelationStoreWriter& ) = delete;
    QueryCorrelationStoreWriter& operator=( const QueryCorrelationStoreWriter& ) = delete;

    void Append( uint64_t nodeId, QueryCorrelationEndpointTag tag, uint64_t ordinal );
    QueryCorrelationBuildStatistics Commit();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class QueryCorrelationStoreReader
{
public:
    QueryCorrelationStoreReader( std::filesystem::path root, std::string sourceFingerprint,
        analysis::PagedRecordOptions options = {} );
    ~QueryCorrelationStoreReader();
    QueryCorrelationStoreReader( const QueryCorrelationStoreReader& ) = delete;
    QueryCorrelationStoreReader& operator=( const QueryCorrelationStoreReader& ) = delete;

    uint64_t Count() const;
    uint64_t BucketCount() const;
    uint64_t CandidateCount( uint64_t nodeId ) const;
    QueryCorrelationCandidatePage ReadCandidates( uint64_t nodeId, uint64_t offset, size_t limit ) const;
    void ValidateAll();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
