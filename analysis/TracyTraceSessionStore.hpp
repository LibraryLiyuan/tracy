#ifndef __TRACYTRACESESSIONSTORE_HPP__
#define __TRACYTRACESESSIONSTORE_HPP__

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionStoreSchemaVersion = 1;
inline constexpr uint32_t TraceSessionCanonicalSchemaVersion = 1;
inline constexpr uint32_t TraceSessionDerivedSchemaVersion = 1;
inline constexpr const char* TraceSessionSuffix = ".jn-trace-session";

enum class TraceSessionState : uint8_t
{
    InventoryBuilding,
    InventoryFailed,
    CapacityPreflight,
    CanonicalBuilding,
    CanonicalPaused,
    CanonicalFailed,
    DerivedBuilding,
    DerivedFailed,
    FinalAuditing,
    CancelledResumable,
    Complete,
    CompleteSourceDegraded,
    InvalidSource,
    InvalidConverterOutput,
    InvalidChecksum,
    InvalidCoreGap,
    InvalidCapacity,
    InsufficientDisk,
    IdentityMismatch
};

struct TraceSessionSourceIdentity
{
    std::string sha256;
    uint64_t fileSize = 0;
    uint64_t committedRevision = 0;
    uint32_t protocol = 0;
    std::string captureIdentity;
    std::string captureEndState;
    std::string converterSha256;
    std::string configurationHash;
};

struct TraceSessionShard
{
    uint64_t shardId = 0;
    std::string domain;
    int64_t timeBeginNs = 0;
    int64_t timeEndNs = 0;
    uint64_t sourceRecordBegin = 0;
    uint64_t sourceRecordEnd = 0;
    uint64_t recordCount = 0;
    uint64_t uncompressedBytes = 0;
    uint64_t fileBytes = 0;
    std::string codec = "none";
    std::string sha256;
    std::filesystem::path relativePath;
};

struct TraceSessionManifest
{
    uint32_t storeSchema = TraceSessionStoreSchemaVersion;
    uint32_t canonicalSchema = TraceSessionCanonicalSchemaVersion;
    uint32_t derivedSchema = TraceSessionDerivedSchemaVersion;
    std::string sessionId;
    std::string generation;
    TraceSessionState state = TraceSessionState::InventoryBuilding;
    TraceSessionSourceIdentity source;
    std::vector<TraceSessionShard> shards;
    std::string reason;
    bool mandatoryDerivedComplete = false;
    bool auditComplete = false;
};

std::filesystem::path DefaultTraceSessionPath( const std::filesystem::path& streamPath );
std::filesystem::path BuildingTraceSessionPath( const std::filesystem::path& finalPath, const std::string& generation );

bool SaveTraceSessionManifest( const std::filesystem::path& root,
    const TraceSessionManifest& manifest, std::string& error );
std::optional<TraceSessionManifest> LoadTraceSessionManifest(
    const std::filesystem::path& root, std::string& error );

bool WriteTraceSessionShard( const std::filesystem::path& root, const std::string& generation,
    TraceSessionShard& shard, const void* payload, size_t payloadBytes, std::string& error );
bool ReadTraceSessionShardPayload( const std::filesystem::path& root,
    const TraceSessionShard& shard, std::vector<uint8_t>& payload, std::string& error );
bool VerifyTraceSession( const std::filesystem::path& root,
    const TraceSessionManifest& manifest, std::string& error );
bool VerifyTraceSessionSourceIdentity( const std::filesystem::path& sourcePath,
    const TraceSessionSourceIdentity& expected, std::string& error );
bool PublishTraceSession( const std::filesystem::path& buildingPath,
    const std::filesystem::path& finalPath, const TraceSessionManifest& manifest, std::string& error );
bool IsTraceSessionQueryable( const std::filesystem::path& root, std::string& error );

const char* TraceSessionStateName( TraceSessionState state );

}

#endif
