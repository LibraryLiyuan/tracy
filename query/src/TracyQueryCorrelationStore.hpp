#ifndef __TRACYQUERYCORRELATIONSTORE_HPP__
#define __TRACYQUERYCORRELATIONSTORE_HPP__

#include "TracyPagedRecordStore.hpp"
#include "TracyTraceSource.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
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
    uint64_t explicitPasses = 0;
    uint64_t frameIdentities = 0;
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
    size_t maximumNodes = 100000;
};

struct QueryIndexedGfxEvidence
{
    analysis::GfxEvidenceSlice value;
    bool edgeLimitExceeded = false;
    bool nodeLimitExceeded = false;
};

struct QueryIndexedExplicitPass
{
    uint64_t projectionOrdinal = 0;
    uint64_t rawCursorAfter = 0;
    analysis::GfxEntityDto pass;
    std::optional<analysis::GfxEntityDto> segment;
    std::optional<analysis::GpuZoneDto> zone;
    uint64_t commandListId = 0;
    uint64_t frameId = 0;
    uint64_t cameraId = 0;
    uint64_t viewId = 0;
    uint64_t referenceToken = 0;
    uint32_t taxonomyId = 0;
    bool taxonomyStableId = false;
    bool managed = false;
};

enum class QueryIndexedExplicitPassFilterTag : uint8_t
{
    PassSource,
    Taxonomy
};

class QueryExplicitPassSource
{
public:
    virtual ~QueryExplicitPassSource() = default;
    virtual uint64_t GetIndexedExplicitPassCount() const = 0;
    virtual uint64_t GetIndexedManagedExplicitPassCount() const = 0;
    virtual uint64_t GetIndexedExplicitPassMissingGpuZoneCount() const = 0;
    virtual uint64_t GetIndexedExplicitPassUnresolvedTaxonomyCount() const = 0;
    virtual uint64_t GetIndexedManagedExplicitPassMissingGpuZoneCount() const = 0;
    virtual uint64_t GetIndexedManagedExplicitPassUnresolvedTaxonomyCount() const = 0;
    virtual std::vector<QueryIndexedExplicitPass> ScanIndexedExplicitPasses(
        uint64_t offset, size_t limit, const QueryCorrelationReadControl& control ) const = 0;
    virtual std::vector<QueryIndexedExplicitPass> ScanIndexedExplicitPassFilter(
        QueryIndexedExplicitPassFilterTag tag, uint64_t key, uint64_t& candidateOffset,
        size_t limit, const QueryCorrelationReadControl& control ) const = 0;
    virtual std::optional<QueryIndexedExplicitPass> FindIndexedExplicitPass(
        uint64_t entityId, const QueryCorrelationReadControl& control ) const = 0;
};

class QueryCorrelationSource
{
public:
    virtual ~QueryCorrelationSource() = default;
    virtual std::vector<analysis::CorrelatedFrameEventDto> GetIndexedFrameEvents(
        uint64_t frameId, const QueryCorrelationReadControl& control ) const = 0;
    virtual uint64_t GetIndexedFrameIdentityCount() const = 0;
    virtual std::vector<uint64_t> ScanIndexedFrameIdentityIds(
        uint64_t offset, size_t limit, const QueryCorrelationReadControl& control ) const = 0;
    virtual QueryIndexedGfxEvidence GetIndexedGfxEvidence( uint64_t frameId,
        const std::vector<uint64_t>& seedIds, const QueryCorrelationReadControl& control ) const = 0;
    virtual QueryIndexedGfxEvidence GetIndexedExplicitGpuPassEvidence(
        const QueryCorrelationReadControl& control ) const = 0;
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
    void AppendFrameIdentity( uint64_t frameId );
    void AppendExplicitPass( uint64_t entityOrdinal );
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
    uint64_t FrameIdentityCount() const;
    std::vector<uint64_t> ReadFrameIdentityIds( uint64_t offset, size_t limit ) const;
    uint64_t ExplicitPassCount() const;
    std::vector<uint64_t> ReadExplicitPassOrdinals( uint64_t offset, size_t limit ) const;
    uint64_t CandidateCount( uint64_t nodeId ) const;
    QueryCorrelationCandidatePage ReadCandidates( uint64_t nodeId, uint64_t offset, size_t limit ) const;
    void ValidateAll( const std::function<void( QueryCorrelationEndpointTag, uint64_t )>& validate = {} );

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
