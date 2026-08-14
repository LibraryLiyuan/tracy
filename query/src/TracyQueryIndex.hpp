#ifndef __TRACYQUERYINDEX_HPP__
#define __TRACYQUERYINDEX_HPP__

#include "TracyWorkerTraceSource.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace tracy::query
{

inline constexpr uint32_t QueryIndexSchemaVersion = 6;

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
    bool cpuZoneIndex = false;
    bool gpuZoneIndex = false;
    bool gpuMemoryProtocol2 = false;
    bool zoneValidationPrecomputed = false;
    analysis::ZoneValidationSummaryDto zoneValidation;
    std::unordered_map<uint64_t, uint64_t> cpuZonesByThread;
    std::unordered_map<uint32_t, uint64_t> gpuZonesByContext;
};

struct QueryIndexValidation
{
    std::optional<QueryIndexManifest> manifest;
    std::string reason;
};

class QueryIndex
{
public:
    static std::filesystem::path ManifestPath( const std::filesystem::path& tracePath );
    static QueryIndexManifest Build( const std::filesystem::path& tracePath, analysis::WorkerTraceSource::StateCallback stateCallback = {} );
    static QueryIndexValidation Validate( const std::filesystem::path& tracePath, bool deep = false );
    static std::unique_ptr<analysis::TraceSource> Open( const QueryIndexManifest& manifest,
        analysis::WorkerTraceSource::StateCallback stateCallback = {}, std::string fingerprintOverride = {} );
};

}

#endif
