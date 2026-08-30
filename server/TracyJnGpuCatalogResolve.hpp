#ifndef __TRACYJNGPUCATALOGRESOLVE_HPP__
#define __TRACYJNGPUCATALOGRESOLVE_HPP__

#include <cstdint>
#include <functional>

namespace tracy
{

struct JnTraceData;

struct JnGpuCatalogResolvedCounts
{
    uint64_t totalUnresolved = 0;
    uint64_t coreUnresolved = 0;
};

using JnGpuCatalogDescriptorAnonymizer = std::function<uint64_t( uint64_t )>;

// Resolve pointer-bearing Catalog records against the authoritative Resource
// lifetime intervals for one connection generation. The function mutates only
// telemetry data and is shared by the live/full Worker and Session conversion.
JnGpuCatalogResolvedCounts ResolveJnGpuCatalogGenerationData( JnTraceData& data,
    uint64_t generation, const JnGpuCatalogDescriptorAnonymizer& anonymizeDescriptor );

}

#endif
