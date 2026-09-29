#include "TracyMemoryAnalysis.hpp"
#include <iostream>
#include <stdexcept>
using namespace tracy::analysis;
int main()
{
    try {
        const std::vector<GpuMemoryCpuZoneInput> scopes{
            {1,GpuMemoryRequestMarker,"outer","GTMEM1|SCOPE|label=7|frame=1",1,0,1000},
            {2,GpuMemoryRequestMarker,"inner","GTMEM1|SCOPE|label=8|frame=1",1,10,20},
            {3,GpuMemoryRequestMarker,"future","GTMEM1|SCOPE|label=9|frame=1",1,50,60}};
        std::vector<GpuMemoryAllocationInput> allocations;
        for(const auto time:{30,15,20,1001}) {
            GpuMemoryAllocationInput value; value.allocationId=uint64_t(time); value.thread=1;
            value.allocationNs=time; value.size=16; value.poolName="GPU D3D12 Physical Resource";
            allocations.push_back(value);
        }
        const auto result=BuildGpuMemoryAttribution(scopes,{},allocations);
        if(result.allocations.size()!=4) throw std::runtime_error("allocation records lost");
        if(result.allocations[0].requestLabelId!=7) throw std::runtime_error("expired inner scope hides live outer allocation request");
        if(result.allocations[1].requestLabelId!=8 || result.allocations[2].requestLabelId!=8)
            throw std::runtime_error("innermost inclusive boundary scope changed");
        if(result.allocations[3].requestLabelId.has_value()) throw std::runtime_error("out-of-scope allocation misattributed");
        std::cout<<"GPU memory request scope passed\n"; return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
