#ifndef __TRACYTRACESESSIONCANONICAL_HPP__
#define __TRACYTRACESESSIONCANONICAL_HPP__

#include "TracyTraceSessionInventory.hpp"
#include "TracyTraceSessionStore.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace tracy::analysis
{

using TraceSessionCanonicalCancel = bool ( * )( void* userData );
using TraceSessionCanonicalDiskSpaceProbe = bool ( * )( const std::filesystem::path& path,
    uint64_t& capacity, uint64_t& available, void* userData, std::string& error );

enum class TraceSessionCanonicalBuildResult : uint8_t
{
    Complete,
    CancelledResumable,
    Failed
};

enum class TraceSessionCanonicalRecordKind : uint8_t
{
    ProtocolEvent = 1,
    TransportRecord = 2,
    ProtocolFrame = 3
};

struct TraceSessionCanonicalRecord
{
    TraceSessionCanonicalRecordKind kind = TraceSessionCanonicalRecordKind::ProtocolEvent;
    uint8_t type = 0;
    TraceSessionProtocolDomain domain = TraceSessionProtocolDomain::Other;
    bool hasSemanticTime = false;
    uint32_t flags = 0;
    uint32_t threadContext = 0;
    uint32_t variablePayloadBytes = 0;
    uint64_t sourceSequence = 0;
    uint64_t journalMonotonicNs = 0;
    uint64_t protocolFrameOrdinal = 0;
    int64_t semanticTime = 0;
    std::span<const uint8_t> payload;
};

using TraceSessionCanonicalRecordVisitor = bool ( * )(
    const TraceSessionCanonicalRecord& record, void* userData, std::string& error );

struct TraceSessionCanonicalAudit
{
    uint64_t protocolFrames = 0;
    uint64_t protocolEvents = 0;
    uint64_t protocolEncodedBytes = 0;
    uint64_t transportRecords = 0;
    uint64_t transportPayloadBytes = 0;
    uint64_t semanticTimeEvents = 0;
    std::array<TraceSessionProtocolEventStats, TraceSessionProtocolInventory::EventTypeCapacity> events {};
    std::array<TraceSessionProtocolEventStats,
        size_t( TraceSessionProtocolDomain::Count )> domains {};
};

struct TraceSessionCanonicalOptions
{
    uint64_t targetShardBytes = 256ull * 1024 * 1024;
    uint64_t softShardBytes = 384ull * 1024 * 1024;
    uint64_t hardShardBytes = 512ull * 1024 * 1024;
    uint64_t minimumShardSpanNs = 5ull * 1000 * 1000 * 1000;
    uint64_t maximumShardSpanNs = 30ull * 1000 * 1000 * 1000;
    uint64_t softMemoryBytes = 12ull * 1024 * 1024 * 1024;
    uint64_t hardMemoryBytes = 16ull * 1024 * 1024 * 1024;
    uint64_t minimumFreeReserveBytes = 64ull * 1024 * 1024 * 1024;
    uint32_t minimumFreeReservePercent = 10;
    TraceSessionCanonicalDiskSpaceProbe diskSpaceProbe = nullptr;
    void* diskSpaceUserData = nullptr;
    bool resume = true;
    TraceSessionCanonicalCancel shouldCancel = nullptr;
    void* cancelUserData = nullptr;
};

TraceSessionCanonicalBuildResult BuildTraceSessionCanonical( const std::filesystem::path& sourcePath,
    const std::filesystem::path& sessionRoot, const std::string& generation,
    const TraceSessionInventory& inventory, const TraceSessionCanonicalOptions& options,
    TraceSessionManifest& manifest, std::string& error );

// The record payload is a view into a bounded shard buffer and is valid only
// for the duration of the callback. The reader validates the shard checksum,
// framing, domain, record count and source range before returning success.
bool VisitTraceSessionCanonicalShard( const std::filesystem::path& sessionRoot,
    const TraceSessionShard& shard, TraceSessionCanonicalRecordVisitor visitor,
    void* userData, std::string& error );
bool AuditTraceSessionCanonical( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionInventory& inventory,
    TraceSessionCanonicalAudit& audit, std::string& error );

}

#endif
