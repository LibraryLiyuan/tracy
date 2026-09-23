#ifndef __TRACYQUERYGPUPASSRESOURCESTORE_HPP__
#define __TRACYQUERYGPUPASSRESOURCESTORE_HPP__

#include "TracyJnData.hpp"
#include "TracyJnGpuCatalog.hpp"
#include "TracyAnalysisExternalSort.hpp"
#include "TracyPagedRecordStore.hpp"
#include "TracyQueryGpuReferenceStore.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace tracy::query
{

struct QueryGpuPassMapping
{
    uint64_t evidencePassId = 0;
    uint64_t queryPassId = 0;
};

class QueryGpuPassResourceInput
{
public:
    virtual ~QueryGpuPassResourceInput() = default;
    virtual uint64_t MappingCount() const = 0;
    virtual QueryGpuPassMapping Mapping( uint64_t ordinal ) const = 0;
    virtual uint64_t RangeCount() const = 0;
    virtual JnGpuRangeSetRecordV1 Range( uint64_t ordinal ) const = 0;
    virtual uint64_t UseCount() const = 0;
    virtual QueryGpuReferenceResolvedUseRecord Use( uint64_t ordinal ) const = 0;
    virtual uint64_t ResolveCatalogResource( uint64_t pointerToken, int64_t time ) const = 0;
    virtual uint64_t ResourceAllocation( uint64_t resourceId ) const = 0;
    virtual uint64_t AllocationBytesAtEnd( uint64_t allocationId ) const = 0;
};

struct QueryGpuPassResourceBuildStatistics
{
    analysis::AnalysisExternalSortStatistics factSort;
    analysis::AnalysisExternalSortStatistics evidenceSort;
    analysis::AnalysisExternalSortStatistics expandedSort;
    analysis::AnalysisExternalSortStatistics rangeSort;
    analysis::AnalysisExternalSortStatistics useSort;
    analysis::AnalysisExternalSortStatistics allocationSort;
    uint64_t inputMs = 0;
    uint64_t mappingSortMs = 0;
    uint64_t factSortMs = 0;
    uint64_t evidenceSortMs = 0;
    uint64_t expansionMs = 0;
    uint64_t expandedSortMs = 0;
    uint64_t rangeSortMs = 0;
    uint64_t useSortMs = 0;
    uint64_t summaryMs = 0;
    uint64_t allocationSortMs = 0;
    uint64_t allocationAggregateMs = 0;
    uint64_t publishMs = 0;
};

struct QueryGpuPassResourceSummary
{
    uint64_t passId = 0;
    uint64_t resourceCount = 0;
    uint64_t allocationCount = 0;
    uint64_t directRangeBytes = 0;
    uint64_t referencedPhysicalBytes = 0;
    uint64_t rangedResourceCount = 0;
    uint64_t rangeCount = 0;
    uint64_t useCount = 0;
};

static_assert( sizeof( QueryGpuPassResourceSummary ) == 64 );

struct QueryGpuPassResourceReadStats
{
    uint64_t rangeBlockLoads = 0;
    uint64_t useBlockLoads = 0;
};

struct QueryGpuPassRangeData
{
    JnGpuRangeSetRecordV1 record {};
    uint64_t generation = 0;
};

struct QueryGpuPassRangePage
{
    std::vector<QueryGpuPassRangeData> values;
    uint64_t total = 0;
};

struct QueryGpuPassUsePage
{
    std::vector<QueryGpuReferenceResolvedUseRecord> values;
    uint64_t total = 0;
};

struct QueryGpuPassIdPage
{
    std::vector<uint64_t> values;
    uint64_t total = 0;
};

class QueryGpuPassResourceSource
{
public:
    virtual ~QueryGpuPassResourceSource() = default;
    virtual std::optional<QueryGpuPassResourceSummary> GetQueryGpuPassResourceSummary( uint64_t passId ) const = 0;
    virtual QueryGpuPassRangePage GetQueryGpuPassRanges( uint64_t passId, uint64_t offset, size_t limit ) const = 0;
    virtual QueryGpuPassUsePage GetQueryGpuPassUses( uint64_t passId, uint64_t offset, size_t limit ) const = 0;
    virtual QueryGpuPassIdPage GetQueryGpuPassEvidenceIds( uint64_t passId, uint64_t offset, size_t limit ) const = 0;
};

class QueryGpuPassResourceStore
{
public:
    static void Build( const std::filesystem::path& root, std::string sourceFingerprint,
        const QueryGpuPassResourceInput& input, analysis::PagedRecordOptions options = {},
        QueryGpuPassResourceBuildStatistics* statistics = nullptr );
};

class QueryGpuPassResourceStoreReader
{
public:
    QueryGpuPassResourceStoreReader( std::filesystem::path root, std::filesystem::path catalogRoot,
        std::filesystem::path gpuReferenceRoot, std::string sourceFingerprint,
        analysis::PagedRecordOptions options = {} );
    ~QueryGpuPassResourceStoreReader();
    QueryGpuPassResourceStoreReader( const QueryGpuPassResourceStoreReader& ) = delete;
    QueryGpuPassResourceStoreReader& operator=( const QueryGpuPassResourceStoreReader& ) = delete;

    std::optional<QueryGpuPassResourceSummary> Summary( uint64_t passId ) const;
    uint64_t SummaryCount() const;
    QueryGpuPassResourceSummary SummaryAt( uint64_t ordinal ) const;
    QueryGpuPassRangePage Ranges( uint64_t passId, uint64_t offset, size_t limit ) const;
    QueryGpuPassUsePage Uses( uint64_t passId, uint64_t offset, size_t limit ) const;
    QueryGpuPassIdPage EvidencePassIds( uint64_t passId, uint64_t offset, size_t limit ) const;
    QueryGpuPassResourceReadStats ReadStatistics() const;
    void ValidateAll() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
