#ifndef __TRACYGPUSCANTYPES_HPP__
#define __TRACYGPUSCANTYPES_HPP__
#include "TracyGpuAnalysis.hpp"
namespace tracy::analysis
{
// Query scan projections only. No on-disk GPU store or Session backend.
struct GpuAnalysisResourceSummary
{
    uint64_t generation = 0;
    uint64_t resourceId = 0;
    uint64_t allocationId = 0;
    uint64_t capacityBytes = 0;
    uint64_t allocationOffsetBytes = 0;
    uint64_t createTime = 0;
    uint64_t destroyTime = 0;
    uint64_t nameHash = 0;
    uint32_t createCallsiteId = 0;
    uint32_t definitionRevision = 0;
    uint32_t nameOriginalLength = 0;
    uint64_t viewCount = 0;
    uint64_t logicalBindingCount = 0;
    uint64_t partCount = 0;
    uint64_t rangeCount = 0;
    uint64_t relationCount = 0;
    uint64_t vgRecordCount = 0;
    uint64_t allocationResourceCount = 0;
    uint16_t primaryKind = 0;
    uint8_t resourceClass = 0;
    uint8_t memoryDomain = 0;
    uint8_t allocationKind = 0;
    uint8_t nameProvenance = 0;
    uint8_t stackProvenance = 0;
    uint8_t exactness = 0;
    bool openBoundary = false;
    bool aliveAtEnd = false;
    bool hasAliasGroup = false;
    std::string name;
};

struct GpuAnalysisRangeStoreEntry
{
    uint64_t resourceId = 0;
    uint64_t generation = 0;
    JnGpuRangeSetRecordV1 record {};
};


}
#endif
