#ifndef __TRACYNEUTRALAGGREGATESTORE_HPP__
#define __TRACYNEUTRALAGGREGATESTORE_HPP__

#include "TracyDeterministicScanTypes.hpp"
#include "TracyAnalysisWriterLease.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t NeutralAggregateStoreSchemaVersion = 1;
inline constexpr uint32_t NeutralScanAlgorithmVersion = 2;
inline constexpr const char* NeutralScanAlgorithmId = "native-scan-v2";

enum class NeutralAggregateState : uint8_t
{
    Building,
    CancelledResumable,
    Complete,
    Invalid
};

struct NeutralAggregateIdentity
{
    std::string traceStrongId;
    std::string queryExecutableSha256;
    std::string querySchema;
    std::string scanAlgorithm = NeutralScanAlgorithmId;
    uint32_t aggregateSchema = NeutralAggregateSchemaVersion;
};

struct NeutralAggregateRun
{
    uint64_t runId = 0;
    std::string domain;
    std::string kind;
    std::filesystem::path relativePath;
    uint64_t recordCount = 0;
    uint64_t fileBytes = 0;
    std::string sha256;
};

struct NeutralAggregateDomain
{
    std::string domain;
    bool present = false;
    std::string status = "absent";
    uint64_t inputCount = 0;
    uint64_t outputCount = 0;
    std::string checksum;
    std::string unavailableReason;
};

struct NeutralAggregateManifest
{
    uint32_t storeSchema = NeutralAggregateStoreSchemaVersion;
    NeutralAggregateIdentity identity;
    std::string aggregateIdentity;
    std::string generation;
    NeutralAggregateState state = NeutralAggregateState::Building;
    uint64_t lastAccessUnixNs = 0;
    bool pinned = false;
    uint32_t reportReferenceCount = 0;
    std::vector<NeutralAggregateDomain> domains;
    bool qualityComplete = false;
    uint64_t unreportedGapCount = 0;
    std::vector<std::string> qualityFindings;
    std::vector<NeutralAggregateRun> runs;
    bool completed = false;
    std::string reason;
};

struct NeutralAggregateCheckpoint
{
    uint32_t storeSchema = NeutralAggregateStoreSchemaVersion;
    std::string generation;
    std::string stage;
    std::string sourceCursor;
    uint64_t sourceEvents = 0;
    uint64_t outputRuns = 0;
    uint64_t memoryBytes = 0;
    uint64_t diskBytes = 0;
    uint64_t progressNumerator = 0;
    uint64_t progressDenominator = 0;
    bool resumable = false;
};

struct NeutralAggregatePruneResult
{
    uint64_t bytesBefore = 0;
    uint64_t bytesAfter = 0;
    uint64_t removedBytes = 0;
    uint64_t removedEntries = 0;
    bool limitBlockedByProtectedEntries = false;
};

using NeutralAggregateWriterLease = AnalysisWriterLease;

std::string ComputeNeutralAggregateIdentity( const NeutralAggregateIdentity& identity );
std::filesystem::path NeutralAggregateCachePath( const std::filesystem::path& cacheRoot,
    const NeutralAggregateIdentity& identity );

bool AcquireNeutralAggregateWriterLease( const std::filesystem::path& storeRoot,
    NeutralAggregateWriterLease& lease, std::string& error );

bool SaveNeutralAggregateManifest( const std::filesystem::path& storeRoot,
    const NeutralAggregateManifest& manifest, std::string& error );
std::optional<NeutralAggregateManifest> LoadNeutralAggregateManifest(
    const std::filesystem::path& storeRoot, std::string& error );
bool CanReuseNeutralAggregate( const NeutralAggregateManifest& manifest,
    const NeutralAggregateIdentity& identity );

bool WriteNeutralAggregateRun( const std::filesystem::path& storeRoot,
    NeutralAggregateRun& run, const void* payload, size_t payloadBytes, std::string& error );
bool ReadNeutralAggregateRun( const std::filesystem::path& storeRoot,
    const NeutralAggregateRun& run, std::vector<uint8_t>& payload, std::string& error );

bool SaveNeutralAggregateCheckpoint( const std::filesystem::path& storeRoot,
    const NeutralAggregateCheckpoint& checkpoint, std::string& error );
std::optional<NeutralAggregateCheckpoint> LoadNeutralAggregateCheckpoint(
    const std::filesystem::path& storeRoot, std::string& error );

bool VerifyNeutralAggregate( const std::filesystem::path& storeRoot,
    const NeutralAggregateManifest& manifest, std::string& error );
bool PruneNeutralAggregateCache( const std::filesystem::path& cacheRoot,
    uint64_t maximumBytes, const std::vector<std::filesystem::path>& activeStoreRoots,
    NeutralAggregatePruneResult& result, std::string& error );

const char* NeutralAggregateStateName( NeutralAggregateState state );

}

#endif
