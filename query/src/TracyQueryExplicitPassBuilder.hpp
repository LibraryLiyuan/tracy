#ifndef __TRACYQUERYEXPLICITPASSBUILDER_HPP__
#define __TRACYQUERYEXPLICITPASSBUILDER_HPP__

#include "TracyPagedRecordStore.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tracy::query
{

struct QueryExplicitPassEntityFact
{
    uint64_t entityId = 0;
    uint64_t ordinal = 0;
    uint32_t gpuQueryId = 0;
    uint8_t gpuContext = 0;
    uint8_t kind = 0;
    uint8_t flags = 0;
};

struct QueryExplicitPassLinkFact
{
    uint64_t sourceId = 0;
    uint64_t targetId = 0;
    uint64_t ordinal = 0;
    uint8_t relation = 0;
};

struct QueryExplicitPassGpuZoneFact
{
    uint64_t ordinal = 0;
    uint32_t context = 0;
    uint32_t queryId = 0;
    int64_t cpuStartNs = 0;
};

struct QueryExplicitPassBuildInput
{
    uint64_t entityCount = 0;
    uint64_t linkCount = 0;
    uint64_t gpuZoneCount = 0;
    std::vector<uint32_t> taxonomyIds;
    std::function<std::vector<QueryExplicitPassEntityFact>( uint64_t, size_t )> scanEntities;
    std::function<std::vector<QueryExplicitPassLinkFact>( uint64_t, size_t )> scanLinks;
    std::function<std::vector<QueryExplicitPassGpuZoneFact>( uint64_t, size_t )> scanGpuZones;
};

struct QueryExplicitPassBuilderStatistics
{
    uint64_t passes = 0;
    uint64_t relevantLinks = 0;
    uint64_t segmentRequests = 0;
    uint64_t resolvedSegments = 0;
    uint64_t gpuZoneKeys = 0;
};

QueryExplicitPassBuilderStatistics BuildQueryExplicitPassStore(
    const std::filesystem::path& root, const std::filesystem::path& temporaryRoot,
    const std::string& sourceFingerprint, const QueryExplicitPassBuildInput& input,
    analysis::PagedRecordOptions options = {} );

}

#endif
