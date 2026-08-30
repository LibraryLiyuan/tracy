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
    FramePassIndex
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

bool WriteGpuAnalysisDerivedStore( const std::filesystem::path& sidecarPath,
    const GpuAnalysisTraceIdentity& identity, const GpuAnalysisSnapshot& snapshot,
    const GpuAnalysisSidecarControl& control, std::string& generation,
    uint64_t& writtenBytes, std::string& error );
bool WriteGpuAnalysisDerivedStoreAt( const std::filesystem::path& algorithmRoot,
    const GpuAnalysisTraceIdentity& identity, const GpuAnalysisSnapshot& snapshot,
    const GpuAnalysisSidecarControl& control, std::string& generation,
    uint64_t& writtenBytes, std::string& error );

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
    size_t AllocationPageCount() const;
    size_t PassPageCount() const;

    bool LoadResourcePage( size_t page, std::vector<GpuResourceAnalysisRecord>& out, std::string& error ) const;
    bool LoadAllocationPage( size_t page, std::vector<GpuAllocationAnalysisRecord>& out, std::string& error ) const;
    bool LoadPassPage( size_t page, std::vector<GpuPassWorkingSet>& out, std::string& error ) const;

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
