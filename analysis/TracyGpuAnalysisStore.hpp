#ifndef __TRACYGPUANALYSISSTORE_HPP__
#define __TRACYGPUANALYSISSTORE_HPP__

#include "TracyGpuAnalysisCache.hpp"
#include "TracyGpuAnalysisSidecar.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

// Win32 defines FindResource as an A/W macro. The sidecar reader uses the
// domain term intentionally, so keep the C++ API independent of include order.
#ifdef FindResource
#  undef FindResource
#endif

namespace tracy::analysis
{

inline constexpr uint32_t GpuAnalysisStoreSchemaVersion = 1;
inline constexpr uint64_t GpuAnalysisStoreTargetPageBytes = 128ull * 1024 * 1024;

enum class GpuAnalysisStorePageKind : uint8_t
{
    Metadata,
    Resource,
    Allocation,
    Pass,
    Residency,
    Churn,
    ResourcePassIndex,
    FramePassIndex,
    Range,
    ResourceSummary,
    PassSummary,
    StablePassSummary,
    PassChildIndex
};

struct GpuAnalysisStorePage
{
    GpuAnalysisStorePageKind kind = GpuAnalysisStorePageKind::Metadata;
    uint64_t firstIndex = 0;
    uint64_t recordCount = 0;
    uint64_t firstKey = 0;
    uint64_t lastKey = 0;
    uint64_t fileBytes = 0;
    uint64_t checksum = 0;
    std::filesystem::path relativePath;
};

struct GpuAnalysisTypeSummary
{
    uint16_t primaryKind = 0;
    uint64_t liveResourceCount = 0;
    uint64_t resourceCapacityBytes = 0;
};

struct GpuAnalysisStoreManifest
{
    uint32_t schema = GpuAnalysisStoreSchemaVersion;
    std::string algorithmId = GpuAnalysisAlgorithmId;
    std::string generation;
    std::string traceSha256;
    uint64_t traceSize = 0;
    uint64_t resourceCount = 0;
    uint64_t allocationCount = 0;
    uint64_t passCount = 0;
    uint64_t passSummaryCount = 0;
    uint64_t stablePassSummaryCount = 0;
    uint64_t passChildRelationCount = 0;
    uint64_t residencyCount = 0;
    uint64_t churnCount = 0;
    uint64_t resourcePassRelationCount = 0;
    uint64_t framePassRelationCount = 0;
    uint64_t rangeCount = 0;
    uint64_t logicalCount = 0;
    uint64_t catalogRelationCount = 0;
    uint64_t sourceGapResourceCount = 0;
    uint64_t sourceGapReferenceCount = 0;
    uint64_t totalBytes = 0;
    bool complete = false;
    std::string reason;
    std::vector<GpuAnalysisTypeSummary> typeSummaries;
    std::vector<GpuAnalysisStorePage> pages;
};

struct GpuAnalysisResourcePassEntry
{
    uint64_t resourceId = 0;
    uint64_t passId = 0;
    uint8_t inclusive = 0;
    uint8_t reserved[7] {};
};

struct GpuAnalysisFramePassEntry
{
    uint64_t frameId = 0;
    uint64_t passId = 0;
};

struct GpuAnalysisPassChildEntry
{
    uint64_t parentPassId = 0;
    uint64_t childPassId = 0;
};

// Fixed-width, independently paged exact summary. It lets Query/MCP inspect
// counts, hashes and physical rollups without decoding variable-size Pass
// member dictionaries. Exact members remain available through PassResources.
struct GpuAnalysisPassSummary
{
    uint64_t passId = 0;
    uint64_t parentPassId = 0;
    uint64_t frameId = 0;
    uint64_t directResourceCount = 0;
    uint64_t inclusiveResourceCount = 0;
    uint64_t directPhysicalBytes = 0;
    uint64_t inclusivePhysicalBytes = 0;
    uint64_t directResourceHash = 0;
    uint64_t inclusiveResourceHash = 0;
    uint64_t directRangeBytes = 0;
    uint32_t taxonomyId = 0;
    uint32_t unknownRangeResourceCount = 0;
    uint8_t taxonomyLevel = 0;
    uint8_t taxonomyFlags = 0;
    uint8_t complete = 0;
    uint8_t truncated = 0;
};

struct GpuAnalysisPassTaxonomyEntry
{
    uint64_t passId = 0;
    uint64_t frameId = 0;
    uint32_t taxonomyId = 0;
    uint8_t taxonomyLevel = 0;
    uint8_t flags = 0;
    uint8_t reserved[2] {};
};

// Exact rollup for a stable Frame/Taxonomy node. Multiple runtime pass
// instances with the same key are deterministically deduplicated.
struct GpuAnalysisStablePassSummary
{
    uint64_t frameId = 0;
    uint32_t taxonomyId = 0;
    uint32_t parentTaxonomyId = 0;
    uint64_t directResourceCount = 0;
    uint64_t inclusiveResourceCount = 0;
    uint64_t directPhysicalBytes = 0;
    uint64_t inclusivePhysicalBytes = 0;
    uint64_t directResourceHash = 0;
    uint64_t inclusiveResourceHash = 0;
    uint8_t taxonomyLevel = 0;
    uint8_t complete = 0;
    uint8_t truncated = 0;
    uint8_t reserved[5] {};
};

uint64_t GpuAnalysisResourceSetHash( const std::vector<uint64_t>& resources );

// Compact, independently paged projection used by resource listings.  Full
// Resource pages retain history and all enrichment vectors for get/explain,
// while list/search must not deserialize multi-gigabyte Logical/Relation data.
struct GpuAnalysisResourceSummary
{
    uint64_t generation = 0;
    uint64_t resourceId = 0;
    uint64_t allocationId = 0;
    uint64_t capacityBytes = 0;
    uint64_t allocationOffsetBytes = 0;
    uint64_t createTime = 0;
    uint64_t destroyTime = 0;
    uint64_t nameHash = 0;
    uint32_t createCallsiteId = 0;
    uint32_t definitionRevision = 0;
    uint32_t nameOriginalLength = 0;
    uint64_t viewCount = 0;
    uint64_t logicalBindingCount = 0;
    uint64_t partCount = 0;
    uint64_t rangeCount = 0;
    uint64_t relationCount = 0;
    uint64_t vgRecordCount = 0;
    uint64_t allocationResourceCount = 0;
    uint16_t primaryKind = 0;
    uint8_t resourceClass = 0;
    uint8_t memoryDomain = 0;
    uint8_t allocationKind = 0;
    uint8_t nameProvenance = 0;
    uint8_t stackProvenance = 0;
    uint8_t exactness = 0;
    bool openBoundary = false;
    bool aliveAtEnd = false;
    bool hasAliasGroup = false;
    std::string name;
};

struct GpuAnalysisRangeStoreEntry
{
    uint64_t resourceId = 0;
    uint64_t generation = 0;
    JnGpuRangeSetRecordV1 record {};
};

struct GpuAnalysisLogicalStoreEntry
{
    uint64_t resourceId = 0;
    uint64_t generation = 0;
    uint64_t nameGeneration = 0;
    JnGpuCatalogLogicalRecordV1 record {};
};

struct GpuAnalysisCatalogRelationStoreEntry
{
    uint64_t resourceId = 0;
    uint64_t generation = 0;
    JnGpuCatalogRelationRecordV1 record {};
};

struct GpuAnalysisCatalogStringIndexEntry
{
    uint64_t generation = 0;
    uint64_t dataOffset = 0;
    uint32_t stringId = 0;
    uint32_t byteLength = 0;
};

// N30 stores globally pass-id ordered, independently checksummed cache pages
// before publishing the N29-compatible derived store. Only one spool page is
// materialized while the final pass and reverse-index pages are written.
struct GpuAnalysisPassSpool
{
    std::filesystem::path root;
    uint64_t pageCount = 0;
    uint64_t passCount = 0;
    uint64_t directRelationCount = 0;
    uint64_t inclusiveRelationCount = 0;
    uint64_t rangeCount = 0;
    uint64_t logicalCount = 0;
    uint64_t catalogRelationCount = 0;
    uint64_t viewCount = 0;
    uint64_t partCount = 0;
    uint64_t virtualGeometryCount = 0;
    uint64_t sourceGapResourceCount = 0;
    uint64_t sourceGapReferenceCount = 0;
    std::vector<std::filesystem::path> rangeRuns;
    std::vector<std::filesystem::path> logicalRuns;
    std::vector<std::filesystem::path> catalogRelationRuns;
    std::vector<std::filesystem::path> viewRuns;
    std::vector<std::filesystem::path> partRuns;
    std::vector<std::filesystem::path> virtualGeometryRuns;
    std::vector<std::filesystem::path> taxonomyRuns;
    std::vector<std::filesystem::path> stableSummaryRuns;
    uint64_t taxonomyCount = 0;
    uint64_t stableSummaryCount = 0;
    bool reused = false;
};

// Bounded Session Catalog product. Resource and Allocation pages are immutable
// cache pages; the lookup files are compact, sorted fixed-width projections
// used while Pass evidence is resolved without recreating a full snapshot.
struct GpuAnalysisCatalogSpool
{
    std::filesystem::path root;
    std::filesystem::path resourceLookupPath;
    std::filesystem::path allocationLookupPath;
    std::filesystem::path allocationResourceCountPath;
    std::filesystem::path pointerLifetimePath;
    std::filesystem::path stringIndexPath;
    std::filesystem::path stringDataPath;
    GpuAnalysisSnapshot overview;
    std::vector<GpuAnalysisStorePage> pages;
    std::vector<GpuAnalysisTypeSummary> typeSummaries;
    uint64_t resourceCount = 0;
    uint64_t allocationCount = 0;
    uint64_t residencyCount = 0;
    uint64_t churnCount = 0;
    uint64_t pageCount = 0;
    uint64_t peakRecordsInMemory = 0;
    bool reused = false;
};

struct GpuAnalysisStoreWriteStats
{
    uint64_t committedPages = 0;
    uint64_t resumedPages = 0;
    uint64_t rebuiltUncommittedPages = 0;
};

class GpuAnalysisCatalogStringReader
{
public:
    GpuAnalysisCatalogStringReader();
    ~GpuAnalysisCatalogStringReader();
    GpuAnalysisCatalogStringReader( GpuAnalysisCatalogStringReader&& ) noexcept;
    GpuAnalysisCatalogStringReader& operator=( GpuAnalysisCatalogStringReader&& ) noexcept;
    GpuAnalysisCatalogStringReader( const GpuAnalysisCatalogStringReader& ) = delete;
    GpuAnalysisCatalogStringReader& operator=( const GpuAnalysisCatalogStringReader& ) = delete;
    bool Open( const GpuAnalysisCatalogSpool& spool, std::string& error );
    std::string Find( uint64_t generation, uint32_t stringId ) const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class GpuAnalysisCatalogAllocationLookupReader
{
public:
    GpuAnalysisCatalogAllocationLookupReader();
    ~GpuAnalysisCatalogAllocationLookupReader();
    GpuAnalysisCatalogAllocationLookupReader( GpuAnalysisCatalogAllocationLookupReader&& ) noexcept;
    GpuAnalysisCatalogAllocationLookupReader& operator=( GpuAnalysisCatalogAllocationLookupReader&& ) noexcept;
    GpuAnalysisCatalogAllocationLookupReader( const GpuAnalysisCatalogAllocationLookupReader& ) = delete;
    GpuAnalysisCatalogAllocationLookupReader& operator=( const GpuAnalysisCatalogAllocationLookupReader& ) = delete;
    bool Open( const GpuAnalysisCatalogSpool& spool, std::string& error );
    uint64_t ResourceCount( uint64_t allocationId ) const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

bool WriteGpuAnalysisDerivedStore( const std::filesystem::path& sidecarPath,
    const GpuAnalysisTraceIdentity& identity, const GpuAnalysisSnapshot& snapshot,
    const GpuAnalysisSidecarControl& control, std::string& generation,
    uint64_t& writtenBytes, std::string& error );
bool WriteGpuAnalysisDerivedStoreAt( const std::filesystem::path& algorithmRoot,
    const GpuAnalysisTraceIdentity& identity, const GpuAnalysisSnapshot& snapshot,
    const GpuAnalysisSidecarControl& control, std::string& generation,
    uint64_t& writtenBytes, std::string& error );
bool WriteGpuAnalysisDerivedStoreFromPassSpoolAt(
    const std::filesystem::path& algorithmRoot,
    const GpuAnalysisTraceIdentity& identity,
    const GpuAnalysisSnapshot& catalogSnapshot,
    const GpuAnalysisPassSpool& passSpool,
    const std::vector<JnGpuCatalogStringData>& catalogStrings,
    const GpuAnalysisSidecarControl& control,
    std::string& generation, uint64_t& writtenBytes, std::string& error );
bool WriteGpuAnalysisDerivedStoreFromCatalogAndPassSpoolsAt(
    const std::filesystem::path& algorithmRoot,
    const GpuAnalysisTraceIdentity& identity,
    const GpuAnalysisCatalogSpool& catalogSpool,
    const std::vector<GpuResourceAnalysisRecord>& appendedResources,
    const GpuAnalysisPassSpool& passSpool,
    const std::vector<JnGpuCatalogStringData>& catalogStrings,
    const GpuAnalysisSidecarControl& control,
    std::string& generation, uint64_t& writtenBytes, std::string& error,
    GpuAnalysisStoreWriteStats* writeStats = nullptr );
// Creates a new immutable derived generation by hard-linking the currently
// published pages and adding compact Resource Summary pages.  Existing
// generations are never mutated and remain a rollback point.
bool BuildGpuAnalysisResourceSummariesAt( const std::filesystem::path& algorithmRoot,
    std::string_view expectedTraceSha256, uint64_t expectedTraceSize,
    const GpuAnalysisSidecarControl& control, std::string& generation,
    uint64_t& logicalBytes, std::string& error );

class GpuAnalysisStoreReader
{
public:
    static std::shared_ptr<GpuAnalysisStoreReader> Open( const std::filesystem::path& tracePath,
        bool strongIdentity, GpuAnalysisSidecarManifest* sidecarManifest, std::string& error );
    // Opens a derived store whose algorithm root already contains `current`
    // and immutable generations. This is the N30 Session path; unlike Open(),
    // it does not require or synthesize an N29 raw sidecar manifest.
    static std::shared_ptr<GpuAnalysisStoreReader> OpenAt( const std::filesystem::path& algorithmRoot,
        std::string_view expectedTraceSha256, uint64_t expectedTraceSize, std::string& error );

    const GpuAnalysisStoreManifest& Manifest() const { return m_manifest; }
    const GpuAnalysisSnapshot& Overview() const { return m_overview; }

    size_t ResourcePageCount() const;
    size_t ResourceSummaryPageCount() const;
    size_t AllocationPageCount() const;
    size_t PassPageCount() const;

    bool LoadResourcePage( size_t page, std::vector<GpuResourceAnalysisRecord>& out, std::string& error ) const;
    bool LoadResourceSummaryPage( size_t page, std::vector<GpuAnalysisResourceSummary>& out, std::string& error ) const;
    bool LoadAllocationPage( size_t page, std::vector<GpuAllocationAnalysisRecord>& out, std::string& error ) const;
    bool LoadPassPage( size_t page, std::vector<GpuPassWorkingSet>& out, std::string& error ) const;
    bool RangesForResource( uint64_t resourceId, size_t offset, size_t limit,
        std::vector<GpuRangeAnalysisRecord>& out, bool& hasMore, std::string& error ) const;

    std::optional<GpuResourceAnalysisRecord> FindResource( uint64_t resourceId, std::string& error ) const;
    bool FindResources( std::vector<uint64_t> resourceIds, std::vector<GpuResourceAnalysisRecord>& out, std::string& error ) const;
    std::optional<GpuAllocationAnalysisRecord> FindAllocation( uint64_t allocationId, std::string& error ) const;
    std::optional<GpuPassWorkingSet> FindPass( uint64_t passId, std::string& error ) const;
    std::optional<GpuAnalysisPassSummary> FindPassSummary( uint64_t passId, std::string& error ) const;
    std::optional<GpuAnalysisStablePassSummary> FindStablePassSummary(
        uint64_t frameId, uint32_t taxonomyId, std::string& error ) const;
    bool PassResources( uint64_t passId, bool inclusive, size_t offset, size_t limit,
        std::vector<uint64_t>& out, bool& hasMore, std::string& error,
        std::stop_token stopToken = {} ) const;
    bool PassRelationsForResource( uint64_t resourceId, size_t offset, size_t limit,
        std::vector<GpuAnalysisResourcePassEntry>& out, bool& hasMore, std::string& error ) const;
    bool PassesForFrame( uint64_t frameId, size_t offset, size_t limit,
        std::vector<GpuPassWorkingSet>& out, bool& hasMore, std::string& error ) const;
    bool PassesForResource( uint64_t resourceId, size_t offset, size_t limit,
        std::vector<GpuPassWorkingSet>& out, bool& hasMore, std::string& error ) const;

private:
    struct InclusiveCache;
    bool LoadPassesByIds( std::vector<uint64_t> ids, std::vector<GpuPassWorkingSet>& out, std::string& error ) const;
    std::filesystem::path m_root;
    GpuAnalysisCacheIdentity m_identity;
    GpuAnalysisStoreManifest m_manifest;
    GpuAnalysisSnapshot m_overview;
    std::shared_ptr<InclusiveCache> m_inclusiveCache;
};

std::optional<GpuAnalysisStoreManifest> LoadGpuAnalysisStoreManifest(
    const std::filesystem::path& root, std::string& error );
const char* GpuAnalysisStorePageKindName( GpuAnalysisStorePageKind kind );

}

#endif
