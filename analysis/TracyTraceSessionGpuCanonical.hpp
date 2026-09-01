#ifndef __TRACYTRACESESSIONGPUCANONICAL_HPP__
#define __TRACYTRACESESSIONGPUCANONICAL_HPP__

#include "TracyTraceSessionCanonical.hpp"
#include "TracyGpuAnalysisSidecar.hpp"
#include "../server/TracyJnData.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace tracy::analysis
{

inline constexpr size_t TraceSessionGpuCatalogBatchKindCount = 10;

struct TraceSessionTimeTransform
{
    double timerMultiplier = 0;
    int64_t baseTime = 0;
    uint64_t processId = 0;
    bool present = false;

    int64_t ToNanoseconds( int64_t value ) const;
};

bool LoadTraceSessionTimeTransform( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionTimeTransform& timeTransform,
    std::string& error );
std::filesystem::path TraceSessionTimeTransformRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionTimeTransformDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionTimeTransform& timeTransform,
    std::string& error );
bool AuditTraceSessionTimeTransformDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionTimeTransform& timeTransform,
    std::string& error );

struct TraceSessionGpuCanonicalStats
{
    uint64_t controlRecords = 0;
    uint64_t catalogPayloads = 0;
    uint64_t catalogBatches = 0;
    uint64_t referenceSetDefinitions = 0;
    uint64_t referenceSetUses = 0;
    uint64_t expandedReferenceUses = 0;
    uint64_t unresolvedPayloads = 0;
    std::array<uint64_t, TraceSessionGpuCatalogBatchKindCount> catalogRecordCounts {};
    std::array<uint64_t, TraceSessionGpuCatalogBatchKindCount> catalogPayloadBytes {};
};

struct TraceSessionGpuDerivedStats
{
    uint64_t resourceCount = 0;
    uint64_t allocationCount = 0;
    uint64_t passCount = 0;
    uint64_t sourceGapResourceCount = 0;
    uint64_t sourceGapReferenceCount = 0;
    uint64_t writtenBytes = 0;
    uint64_t catalogPageCount = 0;
    uint64_t peakCatalogRecordsInMemory = 0;
    uint64_t committedStorePages = 0;
    uint64_t resumedStorePages = 0;
    uint64_t rebuiltUncommittedStorePages = 0;
    bool usedPagedCatalog = false;
    bool catalogSpoolReused = false;
    bool passSpoolReused = false;
    std::string generation;
};

bool LoadTraceSessionGpuCanonicalData( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, JnTraceData& data,
    TraceSessionTimeTransform& timeTransform, TraceSessionGpuCanonicalStats& stats,
    std::string& error );
// Compatibility/Oracle reader: loads Catalog facts into JnTraceData while the
// unbounded Pass and ResourceSet streams stay on disk. Session production uses
// the internal metadata-only reader plus paged Catalog builders instead.
bool LoadTraceSessionGpuCatalogData( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, JnTraceData& data,
    TraceSessionTimeTransform& timeTransform, TraceSessionGpuCanonicalStats& stats,
    std::string& error );

std::filesystem::path TraceSessionGpuAnalysisRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionGpuAnalysisDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const GpuAnalysisSidecarControl& control,
    TraceSessionGpuDerivedStats& stats, std::string& error );

}

#endif
