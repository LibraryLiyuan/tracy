#ifndef __TRACYTRACESESSIONDERIVED_HPP__
#define __TRACYTRACESESSIONDERIVED_HPP__

#include "TracyGpuAnalysisSidecar.hpp"
#include "TracyTraceSessionCanonical.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
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
    uint64_t gpuSourceGapResources = 0;
    uint64_t gpuSourceGapReferences = 0;
    uint64_t frameSets = 0;
    uint64_t frames = 0;
    uint64_t completeFrames = 0;
    uint64_t frameImages = 0;
    uint64_t frameImageDataEvents = 0;
    uint64_t frameImageEvents = 0;
    uint64_t frameImageBc1Bytes = 0;
    uint64_t jobTypes = 0;
    uint64_t jobs = 0;
    uint64_t jobSchedules = 0;
    uint64_t jobConfigs = 0;
    uint64_t jobDependencies = 0;
    uint64_t jobStages = 0;
    uint64_t cpuZones = 0;
    uint64_t cpuZoneBlocks = 0;
    uint64_t completeCpuZones = 0;
    uint64_t invalidCpuZoneTimings = 0;
    uint64_t cpuZoneSources = 0;
    uint64_t cpuZoneBegins = 0;
    uint64_t cpuZoneEnds = 0;
    uint64_t callsites = 0;
    uint64_t resolvedCallstacks = 0;
    uint64_t callstackEntries = 0;
    uint64_t callstackFrameAddresses = 0;
    uint64_t callstackInlineFrames = 0;
    uint64_t symbols = 0;
    uint64_t symbolCodeBytes = 0;
    uint64_t gpuContexts = 0;
    uint64_t gpuZones = 0;
    uint64_t gpuZoneBlocks = 0;
    uint64_t completeGpuZones = 0;
    uint64_t gpuZoneSources = 0;
    uint64_t gpuZoneBegins = 0;
    uint64_t gpuZoneEnds = 0;
    uint64_t gpuTimeEvents = 0;
    uint64_t gpuCalibrationEvents = 0;
    uint64_t gpuSyncEvents = 0;
    uint64_t memoryPools = 0;
    uint64_t memoryEvents = 0;
    uint64_t memoryEventBlocks = 0;
    uint64_t activeMemoryEvents = 0;
    uint64_t memoryAllocations = 0;
    uint64_t memoryFrees = 0;
    uint64_t memoryDiscards = 0;
    uint64_t memoryUnknownFrees = 0;
    uint64_t sampleEvents = 0;
    uint64_t contextSwitchSampleEvents = 0;
    uint64_t sampleDictionaryEntries = 0;
    uint64_t callstackPayloads = 0;
    uint64_t hardwareSampleEvents = 0;
    uint64_t hardwareSampleAddresses = 0;
    uint64_t sampleBlocks = 0;
    uint64_t contextSwitchRecords = 0;
    uint64_t threadWakeupRecords = 0;
    uint64_t contextSwitchEvents = 0;
    uint64_t completeContextSwitchEvents = 0;
    uint64_t cpuContextSwitchEvents = 0;
    uint64_t completeCpuContextSwitchEvents = 0;
    uint64_t schedulingSourceGaps = 0;
    uint64_t cpuTopologyRecords = 0;
    uint64_t cpuTopologyCpus = 0;
    uint64_t threadSummaries = 0;
    uint64_t cpuUsagePoints = 0;
    uint64_t schedulingThreadBlocks = 0;
    uint64_t schedulingCpuBlocks = 0;
    uint64_t relations = 0;
    uint64_t runtimeDomainStates = 0;
    uint64_t scriptFrames = 0;
    uint64_t scriptStackEvents = 0;
    uint64_t ioRequests = 0;
    uint64_t ioConfigs = 0;
    uint64_t ioStages = 0;
    uint64_t gfxDispatches = 0;
    uint64_t gfxEntities = 0;
    uint64_t gfxLinks = 0;
    uint64_t correlatedFrames = 0;
    uint64_t checkpointWrites = 0;
    uint64_t writerLeaseHeartbeats = 0;
    uint64_t peakPrivateBytes = 0;
    bool softMemoryLimitReached = false;
    bool hardMemoryLimitReached = false;
    std::array<uint64_t, size_t( TraceSessionProtocolDomain::Count )> domains {};
};

struct TraceSessionDerivedCheckpoint
{
    uint32_t schema = 1;
    std::string sourceSha256;
    std::string generation;
    std::string stage;
    uint64_t sequence = 0;
    uint64_t committedStages = 0;
    uint64_t committedRecords = 0;
    uint64_t committedBytes = 0;
    uint64_t previousCheckpointHash = 0;
    uint64_t checkpointHash = 0;
};

struct TraceSessionDerivedControl
{
    std::stop_token stopToken;
    std::function<void( float, const char* )> progress;
    std::function<uint64_t()> queryPrivateBytes;
    uint64_t targetPrivateBytes = 8ull * 1024 * 1024 * 1024;
    uint64_t softPrivateBytes = 12ull * 1024 * 1024 * 1024;
    uint64_t hardPrivateBytes = 16ull * 1024 * 1024 * 1024;
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
std::optional<TraceSessionDerivedCheckpoint> LoadTraceSessionDerivedCheckpoint(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
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
