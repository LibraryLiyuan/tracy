#ifndef __TRACYQUERYGPUREFERENCESTORE_HPP__
#define __TRACYQUERYGPUREFERENCESTORE_HPP__

#include "TracyJnData.hpp"
#include "TracyAnalysisExternalSort.hpp"
#include "TracyPagedRecordStore.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tracy::query
{

class QueryGpuReferenceInput
{
public:
    virtual ~QueryGpuReferenceInput() = default;
    virtual uint64_t PassCount() const = 0;
    virtual uint64_t UseCount() const = 0;
    virtual uint64_t EndCount() const = 0;
    virtual JnGpuReferencePassData Pass( uint64_t ordinal ) const = 0;
    virtual JnGpuReferenceUseData Use( uint64_t ordinal ) const = 0;
    virtual JnGpuReferenceEndData End( uint64_t ordinal ) const = 0;
    virtual uint64_t ResolveCatalogResource( uint64_t pointerToken, int64_t time ) const = 0;
};

struct QueryGpuReferenceResolvedUseRecord
{
    uint64_t resourceId = 0;
    uint64_t passId = 0;
    int64_t passStart = 0;
    int64_t passEnd = 0;
    uint32_t usageMask = 0;
    uint32_t resourceSetId = 0;
    uint8_t flags = 0;
    uint8_t encoding = 0;
    uint8_t reserved[6] {};
};

static_assert( sizeof( QueryGpuReferenceResolvedUseRecord ) == 48 );

struct QueryGpuReferenceLookupRecord
{
    uint64_t resourceId = 0;
    uint64_t packedOrdinals = 0;

    uint32_t SourceUseOrdinal() const { return uint32_t( packedOrdinals >> 32 ); }
    uint32_t ResolvedUseOrdinal() const { return uint32_t( packedOrdinals ); }
    static QueryGpuReferenceLookupRecord Make(
        uint64_t resourceId, uint32_t sourceUseOrdinal, uint32_t resolvedUseOrdinal )
    {
        return { resourceId, uint64_t( sourceUseOrdinal ) << 32 | resolvedUseOrdinal };
    }
};

static_assert( sizeof( QueryGpuReferenceLookupRecord ) == 16 );

struct QueryGpuReferenceUsePage
{
    std::vector<QueryGpuReferenceResolvedUseRecord> values;
    uint64_t total = 0;
};

struct QueryGpuReferenceValidationStats
{
    analysis::AnalysisExternalSortStatistics resourceSort;
    analysis::AnalysisExternalSortStatistics catalogSort;
    uint64_t useBlockLoads = 0;
    uint64_t useDecodedBytes = 0;
    uint64_t sortedReferenceCount = 0;
    uint64_t passAndUseValidationMs = 0;
    uint64_t resourceSourceMs = 0;
    uint64_t resourceSortMs = 0;
    uint64_t resourceCompareMs = 0;
    uint64_t catalogSourceMs = 0;
    uint64_t catalogSortMs = 0;
    uint64_t catalogCompareMs = 0;
    uint64_t cleanupMs = 0;
    uint64_t totalMs = 0;
};

struct QueryGpuReferenceBuildStatistics
{
    analysis::AnalysisExternalSortStatistics passEventSort;
    analysis::AnalysisExternalSortStatistics usePassSort;
    analysis::AnalysisExternalSortStatistics resourceSort;
    analysis::AnalysisExternalSortStatistics catalogSort;
    uint64_t passEventSortMs = 0;
    uint64_t passTimePublishMs = 0;
    uint64_t usePassSortMs = 0;
    uint64_t resolveUsesMs = 0;
    uint64_t resourceSortMs = 0;
    uint64_t resourcePublishMs = 0;
    uint64_t catalogReplayMs = 0;
    uint64_t catalogSortMs = 0;
    uint64_t catalogPublishMs = 0;
};

class QueryGpuReferenceStore
{
public:
    static void Build( const std::filesystem::path& root, std::string sourceFingerprint,
        const QueryGpuReferenceInput& input, analysis::PagedRecordOptions options = {},
        QueryGpuReferenceBuildStatistics* statistics = nullptr );
};

class QueryGpuReferenceStoreReader
{
public:
    QueryGpuReferenceStoreReader( std::filesystem::path root, std::string sourceFingerprint,
        analysis::PagedRecordOptions options = {} );
    ~QueryGpuReferenceStoreReader();
    QueryGpuReferenceStoreReader( const QueryGpuReferenceStoreReader& ) = delete;
    QueryGpuReferenceStoreReader& operator=( const QueryGpuReferenceStoreReader& ) = delete;

    QueryGpuReferenceUsePage UsesByResource( uint64_t resourceId, uint64_t offset, size_t limit ) const;
    QueryGpuReferenceUsePage UsesByCatalogResource( uint64_t resourceId, uint64_t offset, size_t limit ) const;
    QueryGpuReferenceUsePage UsesByPass( uint64_t passId, uint64_t offset, size_t limit ) const;
    uint64_t UseCount() const;
    QueryGpuReferenceResolvedUseRecord UseAt( uint64_t ordinal ) const;
    QueryGpuReferenceValidationStats ValidateAll(
        const std::function<uint64_t( uint64_t, int64_t )>& resolveCatalogResource = {},
        const std::filesystem::path& scratchRoot = {} ) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
