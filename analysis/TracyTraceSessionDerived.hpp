#ifndef __TRACYTRACESESSIONDERIVED_HPP__
#define __TRACYTRACESESSIONDERIVED_HPP__

#include "TracyGpuAnalysisSidecar.hpp"
#include "TracyTraceSessionCanonical.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionDomainIndexSchemaVersion = 1;

struct TraceSessionDerivedStats
{
    uint64_t indexedRecords = 0;
    uint64_t indexedProtocolEvents = 0;
    uint64_t indexedProtocolFrames = 0;
    uint64_t indexedTransportRecords = 0;
    uint64_t semanticTimeRecords = 0;
    int64_t firstSemanticTimeRaw = 0;
    int64_t lastSemanticTimeRaw = 0;
    bool semanticTimePresent = false;
    uint64_t indexBytes = 0;
    uint64_t indexFiles = 0;
    uint64_t gpuResources = 0;
    uint64_t gpuAllocations = 0;
    uint64_t gpuPasses = 0;
    uint64_t frameSets = 0;
    uint64_t frames = 0;
    uint64_t completeFrames = 0;
    uint64_t jobTypes = 0;
    uint64_t jobs = 0;
    uint64_t jobSchedules = 0;
    uint64_t jobConfigs = 0;
    uint64_t jobDependencies = 0;
    uint64_t jobStages = 0;
    std::array<uint64_t, size_t( TraceSessionProtocolDomain::Count )> domains {};
};

struct TraceSessionDerivedControl
{
    std::stop_token stopToken;
    std::function<void( float, const char* )> progress;
    uint64_t minimumFreeBytes = 64ull * 1024 * 1024 * 1024;
};

std::filesystem::path TraceSessionDomainIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
// Opens and verifies the immutable Session index generation. This is the
// bounded metadata path used by SessionTraceSource; it never materializes
// Canonical events or a traditional Worker.
bool LoadTraceSessionDerivedStats( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionDerivedStats& stats,
    std::string& error );
bool BuildTraceSessionMandatoryDerived( const std::filesystem::path& sessionRoot,
    TraceSessionManifest& manifest, const TraceSessionInventory& inventory,
    const TraceSessionDerivedControl& control, TraceSessionDerivedStats& stats,
    std::string& error );
bool AuditTraceSessionFinal( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionInventory& inventory,
    TraceSessionDerivedStats& stats, std::string& error );

}

#endif
