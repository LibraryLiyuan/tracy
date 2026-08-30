#ifndef __TRACYTRACESESSIONGPUCANONICAL_HPP__
#define __TRACYTRACESESSIONGPUCANONICAL_HPP__

#include "TracyTraceSessionCanonical.hpp"
#include "../server/TracyJnData.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace tracy::analysis
{

struct TraceSessionTimeTransform
{
    double timerMultiplier = 0;
    int64_t baseTime = 0;
    bool present = false;

    int64_t ToNanoseconds( int64_t value ) const;
};

struct TraceSessionGpuCanonicalStats
{
    uint64_t controlRecords = 0;
    uint64_t catalogPayloads = 0;
    uint64_t catalogBatches = 0;
    uint64_t referenceSetDefinitions = 0;
    uint64_t referenceSetUses = 0;
    uint64_t expandedReferenceUses = 0;
    uint64_t unresolvedPayloads = 0;
};

bool LoadTraceSessionGpuCanonicalData( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, JnTraceData& data,
    TraceSessionTimeTransform& timeTransform, TraceSessionGpuCanonicalStats& stats,
    std::string& error );

}

#endif
