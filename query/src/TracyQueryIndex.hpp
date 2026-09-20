#ifndef __TRACYQUERYINDEX_HPP__
#define __TRACYQUERYINDEX_HPP__

#include "TracyWorkerTraceSource.hpp"
#include "TracyQueryCatalogStore.hpp"
#include "TracyQueryResourceContext.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tracy::query
{

inline constexpr uint32_t QueryIndexSchemaVersion = 32;
inline constexpr uint8_t QueryIndexZoneExtraTextActive = 1;
inline constexpr uint8_t QueryIndexZoneExtraNameActive = 2;
inline constexpr uint8_t QueryIndexZoneExtraNameResolvedFlag = 4;
constexpr bool QueryIndexZoneExtraNameResolved( uint8_t flags ) noexcept
{
    return ( flags & ( QueryIndexZoneExtraNameActive | QueryIndexZoneExtraNameResolvedFlag ) ) ==
        ( QueryIndexZoneExtraNameActive | QueryIndexZoneExtraNameResolvedFlag );
}

inline constexpr uint16_t QueryIndexCpuZoneTimingCompleteFlag = 1;
inline constexpr uint16_t QueryIndexCpuZoneNameActive = 2;
inline constexpr uint16_t QueryIndexCpuZoneNameResolvedFlag = 4;
inline constexpr uint16_t QueryIndexCpuZoneExtraMissingFlag = 8;
constexpr bool QueryIndexCpuZoneNameResolved( uint16_t flags ) noexcept
{
    return ( flags & ( QueryIndexCpuZoneNameActive | QueryIndexCpuZoneNameResolvedFlag ) ) ==
        ( QueryIndexCpuZoneNameActive | QueryIndexCpuZoneNameResolvedFlag );
}
constexpr bool QueryIndexCpuZoneExtraMissing( uint16_t flags ) noexcept
{
    return ( flags & QueryIndexCpuZoneExtraMissingFlag ) != 0;
}

constexpr bool QueryIndexCpuZoneTimingComplete( int64_t end ) noexcept
{
    return end >= 0;
}

constexpr bool QueryIndexGpuZoneTimingComplete( int64_t cpuEnd, int64_t gpuEnd ) noexcept
{
    return cpuEnd >= 0 && gpuEnd >= 0;
}

struct QueryIndexSection
{
    std::filesystem::path path;
    std::string fingerprint;
    uint64_t bytes = 0;
    uint64_t count = 0;
    uint64_t declaredCount = 0;
    uint32_t recordBytes = 0;
};

struct QueryIndexManifest
{
    std::filesystem::path manifestPath;
    std::filesystem::path dataPath;
    std::string sourceFingerprint;
    std::string dataFingerprint;
    uint64_t sourceBytes = 0;
    uint64_t dataBytes = 0;
    int64_t sourceWriteTime = 0;
    QueryIndexSection zoneExtras;
    QueryIndexSection cpuZones;
    QueryIndexSection gpuZones;
    QueryIndexSection samples;
    QueryIndexSection globalStrings;
    QueryIndexSection globalCallstacks;
    QueryIndexSection globalCallstackFrames;
    QueryIndexSection gpuCatalog;
    QueryIndexSection jobStages;
    QueryIndexSection gfxEntities;
    QueryIndexSection gfxLinks;
    QueryIndexSection relations;
    QueryIndexSection gpuReferencePasses;
    QueryIndexSection gpuReferenceUses;
    QueryIndexSection gpuReferenceEnds;
    QueryIndexSection gpuMemoryCpuZones;
    QueryIndexSection gpuMemorySummaryCpuZones;
    QueryIndexSection gpuMemorySummary;
    uint64_t sampleMainCount = 0;
    uint64_t sampleContextSwitchCount = 0;
    bool cpuZoneIndex = false;
    bool gpuZoneIndex = false;
    bool gpuMemoryProtocol2 = false;
    bool zoneValidationPrecomputed = false;
    analysis::ZoneValidationSummaryDto zoneValidation;
    QueryCatalogValidationResult catalogValidation;
    std::unordered_map<uint64_t, uint64_t> cpuZonesByThread;
    std::unordered_map<uint32_t, uint64_t> gpuZonesByContext;
    std::shared_ptr<void> readerLease;
};

struct QueryIndexValidation
{
    std::optional<QueryIndexManifest> manifest;
    std::string reason;
};

class QueryIndex
{
public:
    static bool PublishPathForTesting( const std::filesystem::path& source, const std::filesystem::path& target,
        std::string& error, const std::function<bool()>& cancelled );
    static bool ProbeOwnerLockForTesting( const std::filesystem::path& tracePath, std::string& error );
    static bool CleanupStaleCandidatesForTesting( const std::filesystem::path& root,
        std::string_view identityPrefix, std::string& error );
    static std::shared_ptr<void> AcquireReaderLeaseForTesting( const std::filesystem::path& path,
        std::string& error );
    static bool CleanupUnreferencedGenerationsForTesting( const std::filesystem::path& root,
        std::string_view identityPrefix, std::string_view currentBaseName,
        std::vector<std::filesystem::path>& retained, std::string& error );
    static std::filesystem::path ManifestPath( const std::filesystem::path& tracePath );
    static std::shared_ptr<QueryResourceContext> CreateResourceContext( const std::filesystem::path& tracePath,
        QueryResourceContextOptions options = {} );
    static QueryIndexManifest Build( const std::filesystem::path& tracePath,
        analysis::WorkerTraceSource::StateCallback stateCallback = {},
        std::shared_ptr<QueryResourceContext> resources = {} );
    static QueryIndexValidation Validate( const std::filesystem::path& tracePath, bool deep = false,
        std::shared_ptr<QueryResourceContext> resources = {} );
    static std::unique_ptr<analysis::TraceSource> Open( const QueryIndexManifest& manifest,
        analysis::WorkerTraceSource::StateCallback stateCallback = {}, std::string fingerprintOverride = {},
        std::shared_ptr<QueryResourceContext> resources = {} );
};

}

#endif
