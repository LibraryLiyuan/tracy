#ifndef __TRACYMEMORYANALYSIS_HPP__
#define __TRACYMEMORYANALYSIS_HPP__

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tracy::analysis
{

inline constexpr const char* GpuD3D12PoolPrefix = "GPU D3D12 ";
inline constexpr const char* GpuMemoryRequestMarker = "GTMEM Request Scope";
inline constexpr const char* GpuMemoryPassMarker = "GTMEM Pass Relations";
inline constexpr const char* GpuMemoryOriginMarker = "GTMEM Allocation Origin";
inline constexpr const char* GpuMemoryResidencyMarker = "GTMEM Residency State";
inline constexpr const char* GpuMemoryProtocolPrefix = "GTMEM1|";
inline constexpr const char* GpuMemoryProtocol2Prefix = "GTMEM2|";

bool IsGpuD3D12PoolName( const std::string& name );

struct MemoryEventKey
{
    uint64_t pool = 0;
    size_t index = 0;

    bool operator==( const MemoryEventKey& other ) const { return pool == other.pool && index == other.index; }
    bool operator<( const MemoryEventKey& other ) const { return pool != other.pool ? pool < other.pool : index < other.index; }
};

struct MemoryEventKeyHash
{
    size_t operator()( const MemoryEventKey& value ) const;
};

struct MemoryEventInput
{
    MemoryEventKey key;
    uint64_t identifier = 0;
    uint64_t size = 0;
    int64_t allocationNs = 0;
    std::optional<int64_t> freeNs;
    uint64_t allocationThread = 0;
    uint64_t freeThread = 0;
    uint32_t allocationCallstack = 0;
    uint32_t freeCallstack = 0;
};

struct MemoryFramePoolSummary
{
    uint64_t pool = 0;
    uint64_t startBytes = 0;
    uint64_t allocatedBytes = 0;
    uint64_t freedBytes = 0;
    uint64_t endBytes = 0;
    uint64_t peakBytes = 0;
    uint64_t startCount = 0;
    uint64_t allocatedCount = 0;
    uint64_t freedCount = 0;
    uint64_t endCount = 0;
    uint64_t peakCount = 0;
};

struct MemoryFrameSnapshot
{
    bool valid = false;
    bool consistent = true;
    bool possibleCaptureBaseline = false;
    int64_t begin = 0;
    int64_t end = 0;
    MemoryFramePoolSummary total;
    std::vector<MemoryFramePoolSummary> pools;
    std::vector<MemoryEventKey> activeAtStart;
    std::vector<MemoryEventKey> activeAtEnd;
    std::vector<MemoryEventKey> allocated;
    std::vector<MemoryEventKey> freed;
    std::vector<MemoryEventKey> transitions;
};

MemoryFrameSnapshot BuildMemoryFrameSnapshot(
    int64_t beginNs,
    int64_t endNs,
    const std::vector<uint64_t>& pools,
    const std::vector<MemoryEventInput>& events,
    bool possibleCaptureBaseline = false );

struct GpuMemoryCpuZoneInput
{
    uint64_t zoneIndex = 0;
    std::string markerName;
    std::string name;
    std::string text;
    uint64_t thread = 0;
    int64_t startNs = 0;
    int64_t endNs = 0;
};

struct GpuMemoryGpuZoneInput
{
    uint64_t zoneIndex = 0;
    std::string name;
    uint64_t thread = 0;
    int64_t cpuStartNs = 0;
    int64_t gpuStartNs = 0;
    int64_t gpuEndNs = 0;
    // Authoritative JN GPU pass relation. Zero means the zone is a taxonomy
    // fallback and must use the legacy name/time pairing path.
    uint64_t referenceToken = 0;
};

struct GpuMemoryAllocationInput
{
    MemoryEventKey key;
    uint64_t allocationId = 0;
    uint64_t size = 0;
    uint64_t thread = 0;
    int64_t allocationNs = 0;
    std::optional<int64_t> freeNs;
    uint32_t allocationCallstack = 0;
    uint32_t freeCallstack = 0;
    std::string poolName;
};

struct GpuMemoryAllocationOrigin
{
    uint64_t allocationId = 0;
    uint64_t connectionId = 0;
    uint64_t cpuZoneIndex = 0;
    uint32_t callstackRequested = 0;
    char layer = 'U';
    char residency = 'U';
    bool replayed = false;
    bool preCapture = false;
    bool callstackEmitted = false;
    bool residencyManaged = false;
};

struct GpuMemoryResidencyEvent
{
    uint64_t allocationId = 0;
    uint64_t frame = 0;
    uint64_t fence = 0;
    uint64_t size = 0;
    uint64_t connectionId = 0;
    uint64_t cpuZoneIndex = 0;
    uint32_t flags = 0;
    uint8_t reason = 0;
    char state = 'U';
    bool replayed = false;
    int64_t timeNs = 0;
};

struct GpuMemoryFragmentation
{
    uint64_t heapId = 0;
    uint64_t capacityBytes = 0;
    uint64_t requestedBytes = 0;
    uint64_t coveredBytes = 0;
    uint64_t aliasedBytes = 0;
    uint64_t freeBytes = 0;
    uint64_t largestFreeBlockBytes = 0;
    uint64_t logicalResourceCount = 0;
    double externalFragmentationRatio = 0;
};

struct GpuMemoryChurn
{
    uint64_t peakPhysicalBytes = 0;
    uint64_t activePhysicalBytes = 0;
    uint64_t createdBytes = 0;
    uint64_t createdCount = 0;
    uint64_t freedBytes = 0;
    uint64_t freedCount = 0;
    uint64_t freedFromBaselineBytes = 0;
    uint64_t freedFromBaselineCount = 0;
};

struct GpuMemoryResidencySummary
{
    uint64_t residentBytes = 0;
    uint64_t residentCount = 0;
    uint64_t evictedBytes = 0;
    uint64_t evictedCount = 0;
    uint64_t unknownBytes = 0;
    uint64_t unknownCount = 0;
};

struct GpuMemoryRequestScope
{
    uint64_t labelId = 0;
    uint64_t frame = 0;
    uint64_t thread = 0;
    int64_t start = 0;
    int64_t end = 0;
    std::string name;
    uint64_t cpuZoneIndex = 0;
};

struct GpuMemoryPassUse
{
    uint64_t allocationId = 0;
    uint32_t usageMask = 0;
    char kind = 'U';
    uint32_t resourceSetId = 0;
    uint8_t encoding = 1;
};

struct GpuMemoryReferencePassInput
{
    uint64_t passId = 0;
    uint64_t parentPassId = 0;
    uint64_t frame = 0;
    uint64_t commandListId = 0;
    uint64_t thread = 0;
    int64_t start = 0;
    int64_t end = 0;
    uint32_t taxonomyId = 0;
    uint32_t totalUseCount = 0;
    uint32_t droppedUses = 0;
    uint8_t taxonomyLevel = 0;
    uint8_t flags = 0;
    bool ended = false;
    std::vector<GpuMemoryPassUse> uses;
};

enum class GpuZonePairing : uint8_t
{
    Missing,
    Exact,
    Ambiguous,
    CaptureBoundary,
    SubmissionUnobserved,
    GpuResultUnavailable,
    DerivedLogicalRollup
};

struct GpuMemoryPass
{
    uint64_t passId = 0;
    uint64_t labelId = 0;
    uint64_t frame = 0;
    uint64_t ordinal = 0;
    uint64_t commandListId = 0;
    uint64_t parentPassId = 0;
    uint64_t thread = 0;
    uint64_t gpuThread = 0;
    int64_t start = 0;
    int64_t end = 0;
    int level = -1;
    uint32_t commandCount = 0;
    uint32_t emittedUseCount = 0;
    uint32_t totalUseCount = 0;
    uint32_t expectedChunks = 0;
    uint32_t parsedChunks = 0;
    uint32_t untrackedReferences = 0;
    uint32_t droppedUses = 0;
    bool truncated = false;
    bool complete = false;
    bool structuredBinary = false;
    uint8_t flags = 0;
    std::string name;
    std::string operations;
    std::vector<GpuMemoryPassUse> uses;
    uint64_t cpuZoneIndex = 0;
    std::optional<uint64_t> gpuZoneIndex;
    GpuZonePairing gpuPairing = GpuZonePairing::Missing;
};

struct GpuMemoryAllocationAttribution
{
    GpuMemoryAllocationInput allocation;
    std::optional<uint64_t> requestLabelId;
    std::vector<size_t> passIndices;
};

struct GpuMemoryLogicalResource
{
    uint64_t logicalResourceId = 0;
    uint64_t physicalAllocationId = 0;
    uint64_t size = 0;
    uint64_t physicalOffset = 0;
    uint32_t primaryOwnerId = 0;
    uint32_t physicalOwnerId = 0;
    uint32_t flags = 0;
    char kind = 'U';
    char segment = 'L';
    std::string name;
};

struct GpuMemoryOwnerRollup
{
    uint32_t taxonomyId = 0;
    uint64_t physicalBytes = 0;
    uint64_t physicalAllocationCount = 0;
    uint64_t logicalResourceCount = 0;
};

struct GpuMemoryWorkingSet
{
    uint64_t frame = 0;
    uint32_t taxonomyId = 0;
    uint64_t referencedPhysicalBytes = 0;
    uint64_t physicalAllocationCount = 0;
    uint64_t logicalResourceCount = 0;
    uint64_t inclusiveReferencedPhysicalBytes = 0;
    uint64_t inclusivePhysicalAllocationCount = 0;
    uint64_t inclusiveLogicalResourceCount = 0;
    std::string provenance;
};

// A reason-coded, deterministic summary for pass resource uses which cannot
// be resolved to a live logical resource and its physical backing at the time
// the pass was recorded.  Occurrence count is deliberately separate from the
// number of unique resource ids so validation does not misreport repeated uses
// as distinct allocations.
struct GpuMemoryUnknownUse
{
    uint64_t allocationId = 0;
    uint64_t physicalAllocationId = 0;
    uint64_t occurrenceCount = 0;
    uint64_t firstFrame = 0;
    uint64_t lastFrame = 0;
    uint64_t firstPassId = 0;
    uint64_t lastPassId = 0;
    bool logicalMetadataPresent = false;
    bool logicalPoolEventPresent = false;
    bool physicalPoolEventPresent = false;
    std::string classification;
};

struct GpuMemoryAttribution
{
    bool protocolPresent = false;
    bool protocol2Present = false;
    bool structuredReferencePresent = false;
    bool complete = true;
    uint64_t captureBoundaryPasses = 0;
    uint64_t submissionUnobservedPasses = 0;
    uint64_t gpuResultUnavailablePasses = 0;
    bool passQualityAggregated = false;
    uint64_t aggregatedIncompleteReferencePasses = 0;
    uint64_t aggregatedStructuredIncompleteReferencePasses = 0;
    uint64_t aggregatedLegacyIncompleteReferencePasses = 0;
    uint64_t aggregatedTruncatedReferencePasses = 0;
    uint64_t aggregatedFailureFlagReferencePasses = 0;
    uint64_t aggregatedCommandListBoundaryPasses = 0;
    uint64_t aggregatedDroppedReferenceUses = 0;
    uint64_t aggregatedWorkingSetCount = 0;
    std::vector<GpuMemoryPass> aggregatedIncompleteReferencePreview;
    std::vector<std::string> warnings;
    std::vector<GpuMemoryRequestScope> requestScopes;
    std::vector<GpuMemoryPass> passes;
    std::vector<GpuMemoryAllocationAttribution> allocations;
    std::vector<GpuMemoryLogicalResource> logicalResources;
    std::vector<GpuMemoryOwnerRollup> ownerRollups;
    std::vector<GpuMemoryWorkingSet> workingSets;
    std::vector<GpuMemoryAllocationOrigin> origins;
    std::vector<GpuMemoryResidencyEvent> residencyEvents;
    std::vector<GpuMemoryFragmentation> fragmentation;
    std::vector<GpuMemoryUnknownUse> unknownUses;
    uint64_t unknownUseOccurrences = 0;
    GpuMemoryChurn churn;
    GpuMemoryResidencySummary residency;
    std::unordered_map<uint64_t, size_t> passById;
    std::unordered_map<uint64_t, size_t> allocationById;
    std::unordered_map<uint64_t, size_t> logicalById;
    std::unordered_map<uint64_t, size_t> physicalOriginById;
    std::unordered_map<uint64_t, size_t> logicalOriginById;
};

struct GpuMemoryPassPage
{
    uint64_t totalPasses = 0;
    uint64_t totalUses = 0;
    bool complete = true;
    std::vector<std::string> warnings;
    std::vector<GpuMemoryPass> passes;
};

struct GpuMemoryRequestScopePage
{
    uint64_t totalScopes = 0;
    std::vector<GpuMemoryRequestScope> scopes;
};

struct GpuMemoryAllocationPageItem
{
    GpuMemoryAllocationAttribution attribution;
    std::optional<GpuMemoryAllocationOrigin> origin;
    std::optional<GpuMemoryLogicalResource> logicalResource;
    uint64_t passRefCount = 0;
    std::vector<uint64_t> passIds;
};

struct GpuMemoryAllocationPage
{
    bool protocolPresent = false;
    bool complete = true;
    bool hasMore = false;
    std::vector<std::string> warnings;
    std::vector<GpuMemoryAllocationPageItem> allocations;
};

struct GpuMemoryEvidenceResource
{
    uint64_t resourceId = 0;
    uint64_t physicalAllocationId = 0;
    uint64_t size = 0;
    uint32_t primaryOwnerId = 0;
    char kind = 'U';
    std::string name;
};

struct GpuMemoryEvidenceSlice
{
    bool protocolPresent = false;
    bool complete = true;
    bool truncated = false;
    uint64_t omittedUses = 0;
    std::vector<std::string> warnings;
    std::vector<GpuMemoryPass> passes;
    std::vector<GpuMemoryEvidenceResource> resources;
};

GpuMemoryAttribution BuildGpuMemoryAttribution(
    const std::vector<GpuMemoryCpuZoneInput>& cpuZones,
    const std::vector<GpuMemoryGpuZoneInput>& gpuZones,
    const std::vector<GpuMemoryAllocationInput>& allocations,
    const std::unordered_set<uint64_t>& submittedCommandLists = {},
    const std::unordered_set<uint64_t>& gpuSegmentReferenceTokens = {},
    const std::vector<GpuMemoryReferencePassInput>& structuredReferencePasses = {},
    int64_t captureEndNs = 0 );

std::string FormatGpuMemoryUsage( uint32_t usageMask );
const char* ToString( GpuZonePairing pairing );

}

#endif
