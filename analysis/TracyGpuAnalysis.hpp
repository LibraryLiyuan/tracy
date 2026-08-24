#ifndef __TRACYGPUANALYSIS_HPP__
#define __TRACYGPUANALYSIS_HPP__

#include "TracyMemoryAnalysis.hpp"
#include "../server/TracyJnData.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <functional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t GpuAnalysisSchemaVersion = 1;
inline constexpr uint32_t GpuAnalysisCacheSchemaVersion = 1;

enum class GpuAnalysisState : uint8_t
{
    NotPresent,
    Building,
    Complete,
    Partial,
    Invalid,
    Cancelled,
    ResourceLimit,
    Failed
};

enum class GpuChurnCandidateKind : uint8_t
{
    AliveAtCaptureEnd,
    LongLived,
    HighChurn,
    RepeatedRecreate,
    OpenCreateBoundary,
    MissingDestroy,
    UntrackedDxgiDelta,
    ResidencyOscillation
};

struct GpuAnalysisManifest
{
    GpuAnalysisState state = GpuAnalysisState::NotPresent;
    uint16_t catalogSchema = 0;
    uint16_t evidenceSchema = 0;
    uint64_t generationCount = 0;
    uint64_t resourceRecordCount = 0;
    uint64_t allocationRecordCount = 0;
    uint64_t viewRecordCount = 0;
    uint64_t logicalRecordCount = 0;
    uint64_t partRecordCount = 0;
    uint64_t relationRecordCount = 0;
    uint64_t rangeRecordCount = 0;
    uint64_t vgRecordCount = 0;
    uint64_t evidenceRecordCount = 0;
    uint64_t unresolvedCount = 0;
    uint64_t invalidRecordCount = 0;
    uint64_t payloadBytes = 0;
    bool transportValid = false;
    bool complete = false;
    std::string reason;
};

struct GpuResourceAnalysisRecord
{
    uint64_t generation = 0;
    uint64_t resourceId = 0;
    uint64_t familyId = 0;
    uint64_t allocationId = 0;
    uint64_t capacityBytes = 0;
    uint64_t allocationOffsetBytes = 0;
    uint64_t createTime = 0;
    uint64_t destroyTime = 0;
    uint64_t lastUpdateTime = 0;
    uint64_t nameHash = 0;
    uint32_t createCallsiteId = 0;
    uint32_t definitionRevision = 0;
    uint32_t declaredUsageMask = 0;
    uint32_t observedUsageMask = 0;
    uint32_t format = 0;
    uint64_t width = 0;
    uint32_t height = 0;
    uint16_t depthOrArraySize = 0;
    uint16_t mipLevels = 0;
    uint16_t primaryKind = 0;
    uint8_t resourceClass = 0;
    uint8_t dimension = 0;
    uint8_t memoryDomain = 0;
    uint8_t allocationKind = 0;
    uint8_t classificationProvenance = 0;
    uint8_t nameProvenance = 0;
    uint8_t stackProvenance = 0;
    uint8_t exactness = 0;
    bool openBoundary = false;
    bool aliveAtEnd = false;
    bool invalid = false;
    std::string name;
    std::vector<size_t> history;
    std::vector<size_t> views;
    std::vector<size_t> parts;
    std::vector<size_t> relations;
    std::vector<size_t> ranges;
};

struct GpuAllocationAnalysisRecord
{
    uint64_t generation = 0;
    uint64_t allocationId = 0;
    uint64_t heapId = 0;
    uint64_t parentAllocationId = 0;
    uint64_t sizeBytes = 0;
    uint64_t offsetBytes = 0;
    uint64_t residentBytes = 0;
    uint64_t createTime = 0;
    uint64_t destroyTime = 0;
    uint64_t lastUpdateTime = 0;
    uint16_t primaryKind = 0;
    uint8_t memoryDomain = 0;
    uint8_t allocationKind = 0;
    uint8_t residencyState = 0;
    uint8_t exactness = 0;
    bool openBoundary = false;
    bool aliveAtEnd = false;
    bool invalid = false;
    std::vector<size_t> history;
    std::vector<uint64_t> resources;
};

struct GpuPassWorkingSet
{
    uint64_t passId = 0;
    uint64_t parentPassId = 0;
    uint64_t frameId = 0;
    uint64_t commandListId = 0;
    int64_t startNs = 0;
    int64_t endNs = 0;
    uint64_t directRangeBytes = 0;
    uint64_t directPhysicalBytes = 0;
    uint64_t inclusivePhysicalBytes = 0;
    uint32_t unknownRangeResourceCount = 0;
    bool complete = false;
    bool truncated = false;
    std::string name;
    std::vector<uint64_t> directResources;
    std::vector<uint64_t> inclusiveResources;
};

struct GpuResidencyInterval
{
    uint64_t allocationId = 0;
    int64_t beginNs = 0;
    int64_t endNs = 0;
    uint64_t residentBytes = 0;
    uint8_t state = 0;
    uint8_t exactness = 0;
};

struct GpuChurnCandidate
{
    GpuChurnCandidateKind kind = GpuChurnCandidateKind::AliveAtCaptureEnd;
    uint64_t resourceId = 0;
    uint64_t allocationId = 0;
    uint64_t bytes = 0;
    uint64_t eventCount = 0;
    double score = 0;
    std::string reason;
};

struct GpuFrameComparison
{
    bool valid = false;
    std::string unavailableReason;
    uint64_t frameA = 0;
    uint64_t frameB = 0;
    int64_t referencedPhysicalDelta = 0;
    std::vector<uint64_t> addedResources;
    std::vector<uint64_t> removedResources;
};

struct GpuAnalysisBudget
{
    uint64_t softBytes = 4ull * 1024 * 1024 * 1024;
    uint64_t hardBytes = 6ull * 1024 * 1024 * 1024;
};

struct GpuAnalysisBuildControl
{
    std::stop_token stopToken;
    std::function<void( float, const char* )> progress;
};

struct GpuAnalysisSnapshot
{
    GpuAnalysisManifest manifest;
    uint64_t engineKnownPhysicalBytes = 0;
    uint64_t engineKnownPhysicalPeakBytes = 0;
    int64_t engineKnownPhysicalPeakTimeNs = 0;
    uint64_t allocationCreateCount = 0;
    uint64_t allocationDestroyCount = 0;
    uint64_t allocatedPhysicalBytes = 0;
    uint64_t freedPhysicalBytes = 0;
    uint64_t residentPhysicalBytes = 0;
    uint64_t logicalCapacityBytes = 0;
    std::vector<GpuResourceAnalysisRecord> resources;
    std::vector<GpuAllocationAnalysisRecord> allocations;
    std::vector<GpuPassWorkingSet> passes;
    std::vector<GpuResidencyInterval> residency;
    std::vector<GpuChurnCandidate> churnCandidates;
    std::unordered_map<uint64_t, size_t> resourceById;
    std::unordered_map<uint64_t, size_t> allocationById;
    std::unordered_map<uint64_t, size_t> passById;

    const GpuResourceAnalysisRecord* FindResource( uint64_t resourceId ) const;
    const GpuAllocationAnalysisRecord* FindAllocation( uint64_t allocationId ) const;
    const GpuPassWorkingSet* FindPass( uint64_t passId ) const;
};

GpuAnalysisSnapshot BuildGpuAnalysisSnapshot( const JnTraceData& data, const GpuMemoryAttribution* attribution = nullptr,
    const GpuAnalysisBudget& budget = {}, const GpuAnalysisBuildControl& control = {} );
GpuFrameComparison CompareGpuFrames( const GpuAnalysisSnapshot& snapshot, uint64_t frameA, uint64_t frameB );

const char* GpuAnalysisStateName( GpuAnalysisState value );
const char* GpuCatalogGenerationStateName( uint8_t value );
const char* GpuPrimaryKindName( uint16_t value );
const char* GpuExactnessName( uint8_t value );
const char* GpuChurnCandidateKindName( GpuChurnCandidateKind value );

}

#endif
