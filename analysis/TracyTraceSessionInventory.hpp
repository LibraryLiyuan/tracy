#ifndef __TRACYTRACESESSIONINVENTORY_HPP__
#define __TRACYTRACESESSIONINVENTORY_HPP__

#include "TracyTraceSessionProtocolInventory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionInventorySchemaVersion = 1;

enum class TraceSessionInventoryPhase : uint8_t
{
    JournalScan = 0,
    SourceHash = 1
};

using TraceSessionInventoryProgress = void ( * )( TraceSessionInventoryPhase phase,
    uint64_t completedBytes, uint64_t totalBytes, void* userData );

enum TraceSessionJournalClass : size_t
{
    SessionBegin = 0,
    ClientToServer,
    ServerToClient,
    Checkpoint,
    SessionEnd,
    Diagnostic,
    Count
};

struct TraceSessionInventoryRecordStats
{
    uint64_t count = 0;
    uint64_t payloadBytes = 0;
    uint64_t committedBytes = 0;

    bool operator==( const TraceSessionInventoryRecordStats& ) const = default;
};

struct TraceSessionInventoryOptions
{
    uint64_t maxPayloadBytes = 64ull * 1024 * 1024;
    uint32_t canonicalEstimatePermille = 1200;
    uint32_t derivedEstimatePermille = 1000;
    uint32_t temporaryEstimatePermille = 500;
    TraceSessionInventoryProgress progress = nullptr;
    void* progressUserData = nullptr;
    std::filesystem::path runDirectory;
    uint64_t runTargetBytes = 64ull * 1024 * 1024;
};

enum class TraceSessionInventoryRunKind : uint8_t
{
    JournalRecord = 1,
    ProtocolDependency = 2
};

struct TraceSessionInventoryRun
{
    TraceSessionInventoryRunKind kind = TraceSessionInventoryRunKind::JournalRecord;
    uint64_t runId = 0;
    uint64_t recordBegin = 0;
    uint64_t recordEnd = 0;
    uint64_t recordCount = 0;
    uint64_t fileBytes = 0;
    std::string sha256;
    std::filesystem::path relativePath;
};

struct TraceSessionInventory
{
    uint32_t schema = TraceSessionInventorySchemaVersion;
    uint32_t protocol = 0;
    std::string captureIdentity;
    std::string sourceSha256;
    uint64_t sourceFileSize = 0;
    uint64_t validSize = 0;
    uint64_t recordCount = 0;
    uint64_t committedRevision = 0;
    uint64_t firstMonotonicNs = 0;
    uint64_t lastMonotonicNs = 0;
    uint32_t prefixCrc32c = 0;
    bool complete = false;
    bool sourceDegraded = false;
    std::string qualityReason;
    bool captureEndMetadataPresent = false;
    uint32_t captureEndReason = 0;
    uint64_t captureEndClientBytes = 0;
    uint64_t captureEndServerBytes = 0;
    std::array<TraceSessionInventoryRecordStats, TraceSessionJournalClass::Count> records {};
    bool protocolInventoryComplete = false;
    TraceSessionProtocolInventory protocolInventory;
    std::vector<TraceSessionInventoryRun> runs;
    // This is deliberately zero for the bounded scanner. Large record/frame
    // directories are emitted as immutable disk runs in later stages.
    uint64_t retainedRecordMetadata = 0;
    uint64_t estimatedCanonicalBytes = 0;
    uint64_t estimatedDerivedBytes = 0;
    uint64_t estimatedTemporaryBytes = 0;
    uint64_t estimatedTotalBuildBytes = 0;
};

struct TraceSessionCapacityPolicy
{
    uint64_t maxSessionStoreBytes = 256ull << 30;
    uint64_t minimumFreeReserveBytes = 64ull << 30;
    uint32_t minimumFreeReservePercent = 10;
};

struct TraceSessionCapacityResult
{
    bool accepted = false;
    uint64_t estimatedBuildBytes = 0;
    uint64_t requiredReserveBytes = 0;
    uint64_t requiredAvailableBytes = 0;
    std::string reason;
};

bool BuildTraceSessionInventory( const std::filesystem::path& sourcePath,
    const TraceSessionInventoryOptions& options, TraceSessionInventory& inventory, std::string& error );
bool SaveTraceSessionInventory( const std::filesystem::path& path,
    const TraceSessionInventory& inventory, std::string& error );
std::optional<TraceSessionInventory> LoadTraceSessionInventory(
    const std::filesystem::path& path, std::string& error );
bool EvaluateTraceSessionCapacity( const TraceSessionInventory& inventory,
    uint64_t volumeCapacityBytes, uint64_t volumeAvailableBytes,
    const TraceSessionCapacityPolicy& policy, TraceSessionCapacityResult& result );
bool VerifyTraceSessionInventoryRuns( const std::filesystem::path& root,
    const TraceSessionInventory& inventory, std::string& error );

}

#endif
