#ifndef __TRACYGPUANALYSISSIDECAR_HPP__
#define __TRACYGPUANALYSISSIDECAR_HPP__

#include "TracyGpuAnalysis.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t GpuAnalysisManifestSchemaVersion = 1;
inline constexpr uint32_t GpuAnalysisRawSchemaVersion = 1;
inline constexpr uint32_t GpuAnalysisDerivedSchemaVersion = 1;
inline constexpr const char* GpuAnalysisAlgorithmId = "n29-v1";
inline constexpr const char* GpuAnalysisSidecarSuffix = ".jn-gpu-resource-analysis";
inline constexpr uint64_t GpuAnalysisTargetShardBytes = 192ull * 1024 * 1024;
inline constexpr uint64_t GpuAnalysisMaximumSidecarBytes = 128ull * 1024 * 1024 * 1024;
inline constexpr uint64_t GpuAnalysisMaximumTemporaryBytes = 8ull * 1024 * 1024 * 1024;
inline constexpr uint64_t GpuAnalysisMinimumFreeBytes = 32ull * 1024 * 1024 * 1024;

enum class GpuAnalysisIdentityState : uint8_t
{
    QuickVerified,
    StrongVerified,
    Pending,
    Mismatch
};

enum class GpuAnalysisSidecarState : uint8_t
{
    Missing,
    RawBuilding,
    RawComplete,
    DerivedBuilding,
    Ready,
    Cancelled,
    Failed
};

struct GpuAnalysisTraceIdentity
{
    std::string sha256;
    uint64_t fileSize = 0;
    int64_t writeTime = 0;
    uint64_t volumeId = 0;
    uint64_t fileIdHigh = 0;
    uint64_t fileIdLow = 0;
    uint64_t headHash = 0;
    uint64_t middleHash = 0;
    uint64_t tailHash = 0;
};

struct GpuAnalysisExactSummary
{
    uint64_t resourceRecordCount = 0;
    uint64_t allocationRecordCount = 0;
    uint64_t passCount = 0;
    uint64_t referenceUseCount = 0;
    uint64_t referenceEndCount = 0;
    uint64_t rangeCount = 0;
    uint64_t relationCount = 0;
    uint64_t viewRecordCount = 0;
    uint64_t logicalRecordCount = 0;
    uint64_t partRecordCount = 0;
    uint64_t vgRecordCount = 0;
    uint64_t evidenceRecordCount = 0;
    uint64_t generationCount = 0;
    uint64_t payloadBytes = 0;
    uint64_t allocationCreateCount = 0;
    uint64_t allocationDestroyCount = 0;
    uint64_t engineKnownPhysicalBytes = 0;
    uint64_t engineKnownPhysicalPeakBytes = 0;
    int64_t engineKnownPhysicalPeakTimeNs = 0;
    uint64_t directResourceSetHash = 0;
    uint64_t catalogChecksum = 0;
    uint64_t unresolvedCount = 0;
    uint64_t invalidRecordCount = 0;
    bool catalogPresent = false;
    bool catalogValid = false;
    bool exact = false;
};

struct GpuAnalysisShard
{
    uint32_t kind = 0;
    uint32_t elementSize = 0;
    uint64_t firstIndex = 0;
    uint64_t recordCount = 0;
    uint64_t payloadBytes = 0;
    uint64_t checksum = 0;
    std::filesystem::path relativePath;
};

struct GpuAnalysisSidecarManifest
{
    uint32_t manifestSchema = GpuAnalysisManifestSchemaVersion;
    uint32_t rawSchema = GpuAnalysisRawSchemaVersion;
    uint32_t derivedSchema = GpuAnalysisDerivedSchemaVersion;
    std::string algorithmId = GpuAnalysisAlgorithmId;
    std::string rawGeneration;
    std::string derivedGeneration;
    GpuAnalysisSidecarState state = GpuAnalysisSidecarState::Missing;
    GpuAnalysisIdentityState identityState = GpuAnalysisIdentityState::Pending;
    GpuAnalysisTraceIdentity identity;
    GpuAnalysisExactSummary summary;
    std::vector<GpuAnalysisShard> shards;
    std::string reason;
    bool rawComplete = false;
    bool derivedComplete = false;
};

struct GpuAnalysisSidecarControl
{
    std::stop_token stopToken;
    std::function<void( float, const char* )> progress;
    uint64_t maximumSidecarBytes = GpuAnalysisMaximumSidecarBytes;
    uint64_t maximumTemporaryBytes = GpuAnalysisMaximumTemporaryBytes;
    uint64_t minimumFreeBytes = GpuAnalysisMinimumFreeBytes;
    uint64_t targetDerivedPageBytes = 128ull * 1024 * 1024;
};

std::filesystem::path GpuAnalysisSidecarPath( const std::filesystem::path& tracePath );
std::filesystem::path GpuAnalysisDerivedPath( const std::filesystem::path& sidecarPath );

GpuAnalysisTraceIdentity ComputeGpuAnalysisQuickIdentity( const std::filesystem::path& tracePath );
GpuAnalysisIdentityState VerifyGpuAnalysisIdentity( const std::filesystem::path& tracePath,
    const GpuAnalysisTraceIdentity& expected, bool strong, std::string& reason );

bool WriteGpuAnalysisRawSidecar( const std::filesystem::path& sidecarPath,
    const GpuAnalysisTraceIdentity& identity, const JnTraceData& data,
    const GpuAnalysisSidecarControl& control, std::string& error );
bool PublishGpuAnalysisSidecar( const std::filesystem::path& stagingPath,
    const std::filesystem::path& finalPath, bool overwrite, std::string& error );

std::optional<GpuAnalysisSidecarManifest> LoadGpuAnalysisSidecarManifest(
    const std::filesystem::path& sidecarPath, std::string& error );
bool LoadGpuAnalysisRawData( const std::filesystem::path& sidecarPath,
    const GpuAnalysisSidecarManifest& manifest, JnTraceData& data,
    const GpuAnalysisSidecarControl& control, std::string& error );

bool BuildGpuAnalysisDerived( const std::filesystem::path& tracePath,
    const GpuAnalysisSidecarControl& control, std::string& error );
std::optional<GpuAnalysisSnapshot> LoadGpuAnalysisSidecarSnapshot(
    const std::filesystem::path& tracePath, bool strongIdentity,
    GpuAnalysisSidecarManifest* manifest, std::string& error );

const char* GpuAnalysisIdentityStateName( GpuAnalysisIdentityState value );
const char* GpuAnalysisSidecarStateName( GpuAnalysisSidecarState value );

}

#endif
