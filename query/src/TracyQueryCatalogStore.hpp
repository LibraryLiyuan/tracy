#ifndef __TRACYQUERYCATALOGSTORE_HPP__
#define __TRACYQUERYCATALOGSTORE_HPP__

#include "TracyOfflineCatalog.hpp"
#include "TracyPagedRecordStore.hpp"
#include "TracyQueryGpuReferenceStore.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tracy::query
{

struct QueryCatalogValidationFinding
{
    std::string code;
    uint64_t count = 0;
};

struct QueryCatalogValidationSample
{
    std::string code;
    std::string source;
    uint64_t generation = 0;
    uint64_t id = 0;
    uint64_t expectedHash = 0;
    uint64_t actualHash = 0;
    uint32_t expectedLength = 0;
    uint32_t actualLength = 0;
};

struct QueryCatalogValidationResult
{
    bool complete = false;
    bool present = false;
    bool originalValid = false;
    bool healthy = false;
    std::vector<QueryCatalogValidationFinding> findings;
    std::vector<QueryCatalogValidationSample> samples;
};

struct QueryCatalogResourceIndexReadStatistics
{
    uint64_t resourceCacheBytes = 0;
    uint64_t resourceBlockLoads = 0;
    uint64_t resourceDecodedBytes = 0;
    uint64_t generationBlockLoads = 0;
    uint64_t generationDecodedBytes = 0;
    uint64_t allocationCacheBytes = 0;
    uint64_t allocationBlockLoads = 0;
    uint64_t allocationDecodedBytes = 0;
    uint64_t allocationGenerationBlockLoads = 0;
    uint64_t allocationGenerationDecodedBytes = 0;
};

struct QueryCatalogStatusData
{
    OfflineCatalogQuality metadata {};
    std::array<uint64_t, OfflineCatalogDomainCount> counts {};
    std::vector<JnGpuCatalogGenerationData> generations;
    std::vector<JnGpuDetailedEvidenceRecordV1> evidencePreview;
    uint64_t recordCount = 0;
    uint64_t payloadBytes = 0;
    uint64_t unresolvedCount = 0;
    bool building = false;
    bool generationsTruncated = false;
    bool evidencePreviewTruncated = false;
};

struct QueryCatalogResourceStateData
{
    JnGpuCatalogResourceRecordV1 latest {};
    uint64_t generation = 0;
    int64_t createTime = 0;
    int64_t destroyTime = 0;
    bool openBoundary = false;
    bool aliveAtEnd = false;
};

struct QueryCatalogResourceAssociationData
{
    uint64_t resourceId = 0;
    uint64_t meshPartCount = 0;
    uint64_t partCount = 0;
    uint64_t viewCount = 0;
    uint64_t rangeCount = 0;
    uint64_t logicalBindingCount = 0;
    uint64_t relationCount = 0;
    uint64_t vgRecordCount = 0;
    uint64_t allocationResourceCount = 0;
    uint8_t hasAliasGroup = 0;
    uint8_t reserved[7] {};
};

static_assert( sizeof( QueryCatalogResourceAssociationData ) == 80 );

struct QueryCatalogAllocationData
{
    JnGpuCatalogAllocationRecordV1 record {};
    uint64_t generation = 0;
};

struct QueryCatalogAllocationSummaryData
{
    JnGpuCatalogAllocationRecordV1 latest {};
    uint64_t generation = 0;
    int64_t createTime = 0;
    int64_t destroyTime = 0;
    int64_t lastUpdateTime = 0;
    uint64_t resourceCount = 0;
    uint8_t openBoundary = 0;
    uint8_t aliveAtEnd = 0;
    uint8_t invalid = 0;
    uint8_t reserved[5] {};
};

struct QueryCatalogAllocationPage
{
    std::vector<QueryCatalogAllocationData> values;
    uint64_t total = 0;
};

struct QueryCatalogViewData
{
    JnGpuCatalogViewRecordV1 record {};
    uint64_t generation = 0;
};

struct QueryCatalogViewPage
{
    std::vector<QueryCatalogViewData> values;
    uint64_t total = 0;
};

struct QueryCatalogPartData
{
    JnGpuCatalogPartRecordV1 record {};
    uint64_t generation = 0;
};

struct QueryCatalogPartPage
{
    std::vector<QueryCatalogPartData> values;
    uint64_t total = 0;
};

struct QueryCatalogVgData
{
    JnGpuCatalogVgRecordV1 record {};
    uint64_t generation = 0;
};

struct QueryCatalogVgPage
{
    std::vector<QueryCatalogVgData> values;
    uint64_t total = 0;
};

struct QueryCatalogRelationData
{
    JnGpuCatalogRelationRecordV1 record {};
    uint64_t generation = 0;
};

struct QueryCatalogRelationPage
{
    std::vector<QueryCatalogRelationData> values;
    uint64_t total = 0;
};

struct QueryCatalogEvidenceData
{
    JnGpuDetailedEvidenceRecordV1 record {};
    uint64_t generation = 0;
};

struct QueryCatalogEvidencePage
{
    std::vector<QueryCatalogEvidenceData> values;
    uint64_t total = 0;
    uint8_t lastState = uint8_t( JnGpuEvidenceState::NotSampled );
    bool hasState = false;
};

struct QueryCatalogGpuMemorySummary
{
    uint64_t livePhysicalBytes = 0;
    uint64_t peakPhysicalBytes = 0;
    int64_t peakTime = 0;
    uint64_t allocationCreateCount = 0;
    uint64_t allocationDestroyCount = 0;
    uint64_t allocatedPhysicalBytes = 0;
    uint64_t freedPhysicalBytes = 0;
};

static_assert( sizeof( QueryCatalogGpuMemorySummary ) == 56 );

struct QueryCatalogResourcePointerInterval
{
    uint64_t pointerToken = 0;
    uint64_t resourceId = 0;
    int64_t begin = 0;
    int64_t end = 0;
};

static_assert( sizeof( QueryCatalogResourcePointerInterval ) == 32 );

struct QueryCatalogPointerIntervalPage
{
    std::vector<QueryCatalogResourcePointerInterval> values;
    uint64_t total = 0;
};

struct QueryCatalogRangeData
{
    JnGpuRangeSetRecordV1 record {};
    uint64_t generation = 0;
};

struct QueryCatalogRangePage
{
    std::vector<QueryCatalogRangeData> values;
    uint64_t total = 0;
};

struct QueryCatalogLogicalData
{
    JnGpuCatalogLogicalRecordV1 record {};
    uint64_t generation = 0;
};

struct QueryCatalogLogicalPage
{
    std::vector<QueryCatalogLogicalData> values;
    uint64_t total = 0;
};

class QueryCatalogStatusSource
{
public:
    virtual ~QueryCatalogStatusSource() = default;
    virtual QueryCatalogStatusData GetQueryCatalogStatus( size_t generationLimit, size_t evidenceLimit ) const = 0;
    virtual const QueryCatalogValidationResult& GetQueryCatalogValidation() const = 0;
};

class QueryCatalogResourceSource
{
public:
    virtual ~QueryCatalogResourceSource() = default;
    virtual uint64_t GetQueryCatalogResourceCount() const = 0;
    virtual QueryCatalogResourceStateData GetQueryCatalogResourceStateAt( uint64_t ordinal ) const = 0;
    virtual std::optional<QueryCatalogResourceStateData> GetQueryCatalogResourceState( uint64_t resourceId ) const = 0;
    virtual std::optional<std::string> GetQueryCatalogResourceName( uint64_t generation, uint32_t stringId ) const = 0;
    virtual QueryCatalogResourceAssociationData GetQueryCatalogResourceAssociationsAt( uint64_t ordinal ) const = 0;
    virtual QueryCatalogAllocationPage GetQueryCatalogResourceAllocations(
        uint64_t resourceId, uint64_t offset, size_t limit ) const = 0;
    virtual QueryCatalogViewPage GetQueryCatalogResourceViews(
        uint64_t resourceId, uint64_t offset, size_t limit ) const = 0;
    virtual QueryCatalogPartPage GetQueryCatalogResourceMeshParts(
        uint64_t resourceId, uint64_t offset, size_t limit ) const = 0;
    virtual QueryCatalogPartPage GetQueryCatalogResourceParts(
        uint64_t resourceId, uint64_t offset, size_t limit ) const = 0;
    virtual QueryCatalogVgPage GetQueryCatalogResourceVgPages(
        uint64_t resourceId, uint64_t offset, size_t limit ) const = 0;
    virtual QueryCatalogRelationPage GetQueryCatalogResourceRtasRelations(
        uint64_t resourceId, uint64_t offset, size_t limit ) const = 0;
    virtual QueryCatalogRelationPage GetQueryCatalogResourceRelations(
        uint64_t resourceId, uint64_t offset, size_t limit ) const = 0;
    virtual QueryCatalogLogicalPage GetQueryCatalogResourceLogicals(
        uint64_t resourceId, uint64_t offset, size_t limit ) const = 0;
    virtual std::optional<QueryCatalogLogicalData> ResolveQueryCatalogLogical(
        uint64_t logicalResourceId, int64_t time ) const = 0;
    virtual QueryCatalogEvidencePage GetQueryCatalogPassEvidence(
        uint64_t passId, uint64_t offset, size_t limit ) const = 0;
    virtual QueryCatalogGpuMemorySummary GetQueryCatalogGpuMemorySummary() const = 0;
    virtual uint64_t ResolveQueryCatalogResourcePointer( uint64_t pointerToken, int64_t time ) const = 0;
    virtual QueryCatalogPointerIntervalPage GetQueryCatalogResourcePointerIntervals(
        uint64_t resourceId, uint64_t offset, size_t limit ) const = 0;
    virtual QueryCatalogRangePage GetQueryCatalogResourceRanges(
        uint64_t resourceId, uint64_t offset, size_t limit ) const = 0;
    virtual QueryCatalogRangePage GetQueryCatalogPassRanges(
        uint64_t passId, uint64_t offset, size_t limit ) const = 0;
    virtual QueryGpuReferenceUsePage GetQueryCatalogResourceUses(
        uint64_t resourceId, uint64_t offset, size_t limit ) const = 0;
};

class QueryCatalogStoreWriter
{
public:
    QueryCatalogStoreWriter( std::filesystem::path root, std::string sourceFingerprint,
        analysis::PagedRecordOptions options = {} );
    ~QueryCatalogStoreWriter();
    QueryCatalogStoreWriter( const QueryCatalogStoreWriter& ) = delete;
    QueryCatalogStoreWriter& operator=( const QueryCatalogStoreWriter& ) = delete;

    void SetMetadata( OfflineCatalogQuality metadata );
    void Begin( OfflineCatalogDomain domain, uint64_t count, uint32_t recordBytes );
    void Append( OfflineCatalogDomain domain, uint64_t offset, const void* records, uint64_t count, uint32_t recordBytes );
    void End( OfflineCatalogDomain domain );
    void BeginStrings( uint64_t count );
    void CatalogString( uint64_t index, uint64_t generation,
        const JnGpuCatalogStringRecordHeaderV1& header, const void* bytes );
    void EndStrings();
    void Finish();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class QueryCatalogStoreReader
{
public:
    QueryCatalogStoreReader( std::filesystem::path root, std::string sourceFingerprint,
        analysis::PagedRecordOptions options = {} );
    ~QueryCatalogStoreReader();
    QueryCatalogStoreReader( const QueryCatalogStoreReader& ) = delete;
    QueryCatalogStoreReader& operator=( const QueryCatalogStoreReader& ) = delete;

    OfflineCatalogQuality Metadata() const;
    uint64_t Count( OfflineCatalogDomain domain ) const;
    void Read( OfflineCatalogDomain domain, uint64_t ordinal, void* output, uint64_t count, uint32_t recordBytes ) const;
    JnGpuCatalogStringData String( uint64_t ordinal ) const;
    QueryCatalogStatusData Status( size_t generationLimit, size_t evidenceLimit ) const;
    void ValidateAll( const std::function<void( std::string_view )>& phase = {},
        const std::filesystem::path& scratchRoot = {} ) const;
    QueryCatalogValidationResult ValidateSemantics( const std::filesystem::path& scratchRoot ) const;
    QueryCatalogValidationResult ValidateSemanticsDetailed( const std::filesystem::path& scratchRoot ) const;
    void BuildResourceIndex( const std::filesystem::path& scratchRoot,
        const std::function<void( std::string_view )>& phase = {},
        QueryCatalogResourceIndexReadStatistics* readStatistics = nullptr ) const;
    bool ResourceIndexReady() const;
    uint64_t ResourceIndexCount() const;
    QueryCatalogResourceStateData ResourceStateAt( uint64_t ordinal ) const;
    std::optional<QueryCatalogResourceStateData> ResourceState( uint64_t resourceId ) const;
    QueryCatalogResourceAssociationData ResourceAssociationsAt( uint64_t ordinal ) const;
    std::optional<QueryCatalogResourceAssociationData> ResourceAssociations( uint64_t resourceId ) const;
    QueryCatalogAllocationPage ResourceAllocations( uint64_t resourceId, uint64_t offset, size_t limit ) const;
    uint64_t AllocationSummaryCount() const;
    QueryCatalogAllocationSummaryData AllocationSummaryAt( uint64_t ordinal ) const;
    QueryCatalogViewPage ResourceViews( uint64_t resourceId, uint64_t offset, size_t limit ) const;
    QueryCatalogPartPage ResourceMeshParts( uint64_t resourceId, uint64_t offset, size_t limit ) const;
    QueryCatalogPartPage ResourceParts( uint64_t resourceId, uint64_t offset, size_t limit ) const;
    QueryCatalogVgPage ResourceVgPages( uint64_t resourceId, uint64_t offset, size_t limit ) const;
    QueryCatalogRelationPage ResourceRtasRelations( uint64_t resourceId, uint64_t offset, size_t limit ) const;
    QueryCatalogRelationPage ResourceRelations( uint64_t resourceId, uint64_t offset, size_t limit ) const;
    QueryCatalogLogicalPage ResourceLogicals( uint64_t resourceId, uint64_t offset, size_t limit ) const;
    std::optional<QueryCatalogLogicalData> ResolveLogical( uint64_t logicalResourceId, int64_t time ) const;
    QueryCatalogEvidencePage PassEvidence( uint64_t passId, uint64_t offset, size_t limit ) const;
    QueryCatalogGpuMemorySummary GpuMemorySummary() const;
    uint64_t ResolveResourcePointer( uint64_t pointerToken, int64_t time ) const;
    QueryCatalogPointerIntervalPage ResourcePointerIntervals(
        uint64_t resourceId, uint64_t offset, size_t limit ) const;
    QueryCatalogRangePage ResourceRanges( uint64_t resourceId, uint64_t offset, size_t limit ) const;
    QueryCatalogRangePage PassRanges( uint64_t passId, uint64_t offset, size_t limit ) const;
    uint64_t RangeCount() const;
    QueryCatalogRangeData RangeAt( uint64_t ordinal ) const;
    std::optional<QueryCatalogAllocationData> AllocationState( uint64_t allocationId ) const;
    // Visits raw allocation facts in original ordinal order. The callback must not re-enter this reader.
    void VisitRawAllocations(
        const std::function<void( uint64_t, const JnGpuCatalogAllocationRecordV1& )>& visit ) const;
    std::optional<std::string> ResourceName( uint64_t generation, uint32_t stringId ) const;
    uint64_t ResourceNameLookupDecodedBytes() const;
    uint64_t ResourcePointerLookupDecodedBytes() const;
    uint64_t ResourcePointerIntervalDecodedBytes() const;
    bool PreparePointerResolutionFastPath();
    uint64_t PointerResolutionFastPathBytes() const;
    uint64_t PointerResolutionFastPathHashSlots() const;
    uint64_t PointerResolutionKeyCount() const;
    std::optional<uint32_t> FindPointerResolutionKeyOrdinal( uint64_t pointerToken ) const;
    uint64_t PointerResolutionKeyAtOrdinal( uint32_t ordinal ) const;
    std::optional<uint32_t> ResolveResourcePointerOrdinal( uint64_t pointerToken, int64_t time ) const;
    uint64_t FastResourceIdAtOrdinal( uint32_t ordinal ) const;

private:
    QueryCatalogValidationResult ValidateSemanticsImpl(
        const std::filesystem::path& scratchRoot, bool detailed ) const;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
