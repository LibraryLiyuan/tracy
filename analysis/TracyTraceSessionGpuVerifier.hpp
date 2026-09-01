#ifndef __TRACYTRACESESSIONGPUVERIFIER_HPP__
#define __TRACYTRACESESSIONGPUVERIFIER_HPP__

#include "TracyTraceSessionInventory.hpp"
#include "TracyTraceSessionStore.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionGpuVerifierSchemaVersion = 1;

struct TraceSessionGpuVerifierControl
{
    std::stop_token stopToken;
    uint64_t maximumBufferedAllocationRecords = 65536;
    uint64_t maximumBufferedDeltas = 65536;
    uint64_t maximumBufferedPassRelations = 1048576;
};

// This is an independently recomputed audit product.  It deliberately stores
// both source and Derived facts so Query/MCP never has to treat the Builder's
// own manifest as proof that the Builder was correct.
struct TraceSessionGpuVerifierReport
{
    uint32_t schema = TraceSessionGpuVerifierSchemaVersion;
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string sessionGeneration;
    std::string gpuGeneration;

    uint64_t sourceGpuCatalogEvents = 0;
    uint64_t sourceCatalogRecordCount = 0;
    uint64_t sourceReferenceRelationCount = 0;
    uint64_t sourcePayloadBytes = 0;
    uint64_t sourceRecordHash = 0;

    uint64_t resourceCount = 0;
    uint64_t allocationCount = 0;
    uint64_t passCount = 0;
    uint64_t rangeCount = 0;
    uint64_t resourcePassRelationCount = 0;
    uint64_t storePageBytes = 0;
    uint64_t resourceCapacityBytes = 0;
    uint64_t livePhysicalBytes = 0;
    uint64_t engineKnownPhysicalPeakBytes = 0;
    int64_t engineKnownPhysicalPeakTimeNs = 0;

    uint64_t directMemberCount = 0;
    uint64_t inclusiveMemberCount = 0;
    uint64_t directMemberHash = 0;
    uint64_t inclusiveMemberHash = 0;
    uint64_t forwardRelationHash = 0;
    uint64_t reverseRelationHash = 0;
    uint64_t verifiedPassCount = 0;
    uint64_t mismatchCount = 0;
    uint64_t reportHash = 0;
    bool complete = false;
    std::string reason;
};

std::filesystem::path TraceSessionGpuVerifierRoot(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest );

bool VerifyTraceSessionGpuDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionInventory& inventory,
    const TraceSessionGpuVerifierControl& control,
    TraceSessionGpuVerifierReport& report, std::string& error );

std::optional<TraceSessionGpuVerifierReport> LoadTraceSessionGpuVerifierReport(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
    std::string& error );

}

#endif
