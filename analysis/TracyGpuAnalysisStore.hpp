#ifndef __TRACYGPUANALYSISSTORE_HPP__
#define __TRACYGPUANALYSISSTORE_HPP__

#include "TracyGpuAnalysisCache.hpp"
#include "TracyGpuAnalysisSidecar.hpp"

#include <filesystem>
#include <memory>
#include <optional>
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
    ResourceSummary
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
    uint64_t sourceGapResourceCount = 0;
    uint64_t sourceGapReferenceCount = 0;
    std::vector<std::filesystem::path> rangeRuns;
    std::vector<std::filesystem::path> logicalRuns;
    std::vector<std::filesystem::path> catalogRelationRuns;
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
    bool PassesForFrame( uint64_t frameId, size_t offset, size_t limit,
        std::vector<GpuPassWorkingSet>& out, bool& hasMore, std::string& error ) const;
    bool PassesForResource( uint64_t resourceId, size_t offset, size_t limit,
        std::vector<GpuPassWorkingSet>& out, bool& hasMore, std::string& error ) const;

private:
    bool LoadPassesByIds( std::vector<uint64_t> ids, std::vector<GpuPassWorkingSet>& out, std::string& error ) const;
    std::filesystem::path m_root;
    GpuAnalysisCacheIdentity m_identity;
    GpuAnalysisStoreManifest m_manifest;
    GpuAnalysisSnapshot m_overview;
};

std::optional<GpuAnalysisStoreManifest> LoadGpuAnalysisStoreManifest(
    const std::filesystem::path& root, std::string& error );
const char* GpuAnalysisStorePageKindName( GpuAnalysisStorePageKind kind );

}

#endif
