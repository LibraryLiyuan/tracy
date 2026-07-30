#ifndef __TRACYMEMORYANALYSIS_HPP__
#define __TRACYMEMORYANALYSIS_HPP__

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tracy::analysis
{

inline constexpr const char* GpuD3D12PoolPrefix = "GPU D3D12 ";
inline constexpr const char* GpuMemoryRequestMarker = "GTMEM Request Scope";
inline constexpr const char* GpuMemoryPassMarker = "GTMEM Pass Relations";
inline constexpr const char* GpuMemoryProtocolPrefix = "GTMEM1|";

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
};

struct GpuMemoryAllocationInput
{
    MemoryEventKey key;
    uint64_t allocationId = 0;
    uint64_t size = 0;
    uint64_t thread = 0;
    int64_t allocationNs = 0;
    std::string poolName;
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
};

enum class GpuZonePairing : uint8_t
{
    Missing,
    Exact,
    Ambiguous
};

struct GpuMemoryPass
{
    uint64_t passId = 0;
    uint64_t labelId = 0;
    uint64_t frame = 0;
    uint64_t ordinal = 0;
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
};

struct GpuMemoryAttribution
{
    bool protocolPresent = false;
    bool complete = true;
    std::vector<std::string> warnings;
    std::vector<GpuMemoryRequestScope> requestScopes;
    std::vector<GpuMemoryPass> passes;
    std::vector<GpuMemoryAllocationAttribution> allocations;
    std::vector<GpuMemoryLogicalResource> logicalResources;
    std::vector<GpuMemoryOwnerRollup> ownerRollups;
    std::vector<GpuMemoryWorkingSet> workingSets;
    std::unordered_map<uint64_t, size_t> passById;
    std::unordered_map<uint64_t, size_t> allocationById;
    std::unordered_map<uint64_t, size_t> logicalById;
};

GpuMemoryAttribution BuildGpuMemoryAttribution(
    const std::vector<GpuMemoryCpuZoneInput>& cpuZones,
    const std::vector<GpuMemoryGpuZoneInput>& gpuZones,
    const std::vector<GpuMemoryAllocationInput>& allocations );

std::string FormatGpuMemoryUsage( uint32_t usageMask );
const char* ToString( GpuZonePairing pairing );

}

#endif
