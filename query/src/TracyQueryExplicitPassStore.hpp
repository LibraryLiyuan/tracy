#ifndef __TRACYQUERYEXPLICITPASSSTORE_HPP__
#define __TRACYQUERYEXPLICITPASSSTORE_HPP__

#include "TracyPagedRecordStore.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tracy::query
{

inline constexpr uint32_t QueryExplicitPassMissingSegment = 0x7fffffffu;
inline constexpr uint32_t QueryExplicitPassStableTaxonomyBit = 0x80000000u;

#pragma pack( push, 1 )
struct QueryExplicitPassRecord
{
    uint32_t entityOrdinal = 0;
    uint32_t segmentOrdinalAndStable = QueryExplicitPassMissingSegment;
    uint64_t commandListId = 0;
    uint64_t frameId = 0;
    uint64_t cameraId = 0;
    uint64_t viewId = 0;
    uint64_t referenceToken = 0;
    uint32_t taxonomyId = 0;

    bool HasSegment() const
    {
        return ( segmentOrdinalAndStable & QueryExplicitPassMissingSegment ) !=
            QueryExplicitPassMissingSegment;
    }
    uint32_t SegmentOrdinal() const
    {
        return segmentOrdinalAndStable & QueryExplicitPassMissingSegment;
    }
    bool TaxonomyStable() const
    {
        return ( segmentOrdinalAndStable & QueryExplicitPassStableTaxonomyBit ) != 0;
    }
};

struct QueryExplicitPassGpuZoneKey
{
    uint64_t queryKey = 0;
    int64_t cpuStartNs = 0;
    uint64_t zoneOrdinal = 0;
};
#pragma pack( pop )

static_assert( sizeof( QueryExplicitPassRecord ) == 52 );
static_assert( sizeof( QueryExplicitPassGpuZoneKey ) == 24 );

enum class QueryExplicitPassFilterTag : uint8_t
{
    PassSource,
    Taxonomy,
    LegacyTaxonomy
};

struct QueryExplicitPassFilterCandidate
{
    QueryExplicitPassFilterTag tag = QueryExplicitPassFilterTag::PassSource;
    uint64_t projectionOrdinal = 0;
};

struct QueryExplicitPassFilterPage
{
    std::vector<QueryExplicitPassFilterCandidate> values;
    uint64_t total = 0;
    bool hasMore = false;
};

struct QueryExplicitPassLocated
{
    uint64_t projectionOrdinal = 0;
    QueryExplicitPassRecord record;
};

struct QueryExplicitPassBuildStatistics
{
    uint64_t passes = 0;
    uint64_t managedPasses = 0;
    uint64_t gpuZoneKeys = 0;
    uint64_t filterRecords = 0;
    uint64_t filterTemporaryBytes = 0;
    uint64_t missingGpuZones = 0;
    uint64_t unresolvedTaxonomies = 0;
};

class QueryExplicitPassStoreWriter
{
public:
    QueryExplicitPassStoreWriter( std::filesystem::path root, std::string sourceFingerprint,
        uint64_t gfxEntityCount, uint64_t gpuZoneCount, uint64_t expectedPasses,
        uint64_t expectedFilterRecords, analysis::PagedRecordOptions options = {} );
    ~QueryExplicitPassStoreWriter();
    QueryExplicitPassStoreWriter( const QueryExplicitPassStoreWriter& ) = delete;
    QueryExplicitPassStoreWriter& operator=( const QueryExplicitPassStoreWriter& ) = delete;

    void AppendPass( const QueryExplicitPassRecord& value, bool managed,
        bool hasGpuZone = true, bool taxonomyResolved = true );
    void SealPasses();
    void AppendFilter( uint64_t key, QueryExplicitPassFilterTag tag, uint64_t projectionOrdinal );
    void AppendGpuZoneKey( const QueryExplicitPassGpuZoneKey& value );
    QueryExplicitPassBuildStatistics Commit();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class QueryExplicitPassStoreReader
{
public:
    QueryExplicitPassStoreReader( std::filesystem::path root, std::string sourceFingerprint,
        uint64_t gfxEntityCount, uint64_t gpuZoneCount, analysis::PagedRecordOptions options = {} );
    ~QueryExplicitPassStoreReader();
    QueryExplicitPassStoreReader( const QueryExplicitPassStoreReader& ) = delete;
    QueryExplicitPassStoreReader& operator=( const QueryExplicitPassStoreReader& ) = delete;

    uint64_t Count() const;
    uint64_t ManagedCount() const;
    uint64_t GpuZoneKeyCount() const;
    uint64_t MissingGpuZoneCount() const;
    uint64_t UnresolvedTaxonomyCount() const;
    uint64_t ManagedMissingGpuZoneCount() const;
    uint64_t ManagedUnresolvedTaxonomyCount() const;
    std::vector<QueryExplicitPassRecord> ReadPasses( uint64_t offset, size_t limit ) const;
    bool IsManaged( uint64_t projectionOrdinal ) const;
    std::optional<QueryExplicitPassLocated> FindByEntityOrdinal( uint32_t entityOrdinal ) const;
    uint64_t FilterCandidateCount( uint64_t key ) const;
    QueryExplicitPassFilterPage ReadFilterCandidates( uint64_t key, uint64_t offset, size_t limit ) const;
    std::optional<uint64_t> FindNearestGpuZone( uint64_t queryKey, int64_t cpuStartNs ) const;
    void ValidateAll();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
