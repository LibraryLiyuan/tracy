#ifndef TRACY_MEMORY_ATTRIBUTION_HPP
#define TRACY_MEMORY_ATTRIBUTION_HPP
#include <cstdint>
#include <string_view>
#include <vector>
namespace tracy
{
struct MemoryAttributionRow
{
    const char* name;
    uint64_t count=0, logicalBytes=0, ownedCapacityBytes=0;
    // Informational lower bound only: excluded from knownOwnedBytes.
    uint64_t hashPayloadEstimateBytes=0;
};
struct MemoryAttribution
{
    std::vector<MemoryAttributionRow> rows;
    uint64_t knownOwnedBytes=0, protocolEvents=0, compressedFrames=0;
    void Add(const char* name,uint64_t count,uint64_t logical,uint64_t owned,uint64_t hashEstimate=0)
    {
        knownOwnedBytes+=owned;
        for(auto& row:rows) if(std::string_view(row.name)==name)
        { row.count+=count; row.logicalBytes+=logical; row.ownedCapacityBytes+=owned; row.hashPayloadEstimateBytes+=hashEstimate; return; }
        rows.push_back({name,count,logical,owned,hashEstimate});
    }
};
}
#endif
