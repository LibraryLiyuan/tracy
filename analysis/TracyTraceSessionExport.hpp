#ifndef __TRACYTRACESESSIONEXPORT_HPP__
#define __TRACYTRACESESSIONEXPORT_HPP__

#include "TracyTraceSessionStore.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tracy::analysis
{

struct TraceSessionExportSelection
{
    std::optional<int64_t> timeBeginNs;
    std::optional<int64_t> timeEndNs;
    std::optional<uint64_t> frameSet;
    std::optional<uint64_t> frameBegin;
    std::optional<uint64_t> frameEnd;
};

struct TraceSessionExportRange
{
    int64_t beginNs = 0;
    int64_t endNs = 0;
    bool frameSelection = false;
    uint64_t frameSet = 0;
    uint64_t frameBegin = 0;
    uint64_t frameEnd = 0;
};

struct TraceSessionExportControl
{
    uint64_t workerMemoryLimitBytes = 16ull * 1024 * 1024 * 1024;
    uint64_t workerFixedOverheadBytes = 512ull * 1024 * 1024;
    uint32_t workerExpansionNumerator = 4;
    uint32_t workerExpansionDenominator = 1;
};

struct TraceSessionExportPlan
{
    TraceSessionExportRange range;
    std::vector<uint64_t> shardIds;
    uint64_t scannedDataShards = 0;
    uint64_t windowSemanticShards = 0;
    uint64_t dependencySourceShards = 0;
    uint64_t windowSemanticRecords = 0;
    uint64_t timelessDependencyRecords = 0;
    uint64_t recordCount = 0;
    uint64_t canonicalUncompressedBytes = 0;
    uint64_t canonicalFileBytes = 0;
    uint64_t estimatedWorkerBytes = 0;
    uint64_t workerMemoryLimitBytes = 0;
    uint64_t workerFixedOverheadBytes = 0;
    uint32_t workerExpansionNumerator = 0;
    uint32_t workerExpansionDenominator = 0;
    bool memoryAllowed = false;
    std::string estimateMethod;
};

bool ResolveTraceSessionExportRange( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionExportSelection& selection,
    TraceSessionExportRange& range, std::string& error );

bool BuildTraceSessionExportPlan( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionExportRange& range,
    const TraceSessionExportControl& control, TraceSessionExportPlan& plan,
    std::string& error );

}

#endif
