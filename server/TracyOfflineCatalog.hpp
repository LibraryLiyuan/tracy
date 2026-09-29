#ifndef TRACY_OFFLINE_CATALOG_HPP
#define TRACY_OFFLINE_CATALOG_HPP
#include "TracyJnData.hpp"
#include <array>
#include <stdexcept>
#define TRACY_OFFLINE_CATALOG_FIXED_DOMAINS(X) \
 X(CatalogControl,gpuCatalogControls,JnGpuCatalogControlData,false) \
 X(CatalogBatch,gpuCatalogBatches,JnGpuCatalogBatchData,true) \
 X(CatalogGeneration,gpuCatalogGenerations,JnGpuCatalogGenerationData,true) \
 X(CatalogResource,gpuCatalogResources,JnGpuCatalogResourceRecordV1,false) \
 X(CatalogAllocation,gpuCatalogAllocations,JnGpuCatalogAllocationRecordV1,false) \
 X(CatalogView,gpuCatalogViews,JnGpuCatalogViewRecordV1,true) \
 X(CatalogLogical,gpuCatalogLogicals,JnGpuCatalogLogicalRecordV1,true) \
 X(CatalogPart,gpuCatalogParts,JnGpuCatalogPartRecordV1,false) \
 X(CatalogRelation,gpuCatalogRelations,JnGpuCatalogRelationRecordV1,true) \
 X(CatalogVg,gpuCatalogVg,JnGpuCatalogVgRecordV1,true) \
 X(CatalogRangeSet,gpuRangeSets,JnGpuRangeSetRecordV1,true) \
 X(CatalogDetailedEvidence,gpuDetailedEvidence,JnGpuDetailedEvidenceRecordV1,true)
namespace tracy {
#pragma pack(push,1)
struct OfflineCatalogString {uint64_t generation=0;JnGpuCatalogStringRecordHeaderV1 header{};std::array<unsigned char,256> bytes{};};
#pragma pack(pop)
static_assert(sizeof(OfflineCatalogString)==8+sizeof(JnGpuCatalogStringRecordHeaderV1)+256);
enum class OfflineCatalogDomain {
#define X(name,field,type,mut) name,
 TRACY_OFFLINE_CATALOG_FIXED_DOMAINS(X)
#undef X
 CatalogString,Count
};
inline constexpr size_t OfflineCatalogDomainCount=size_t(OfflineCatalogDomain::Count);
struct OfflineCatalogDescriptor {const char* name;uint32_t width;bool mutableRows;};
inline constexpr OfflineCatalogDescriptor OfflineCatalogDescriptors[]={
#define X(name,field,type,mut) {#field,sizeof(type),mut},
 TRACY_OFFLINE_CATALOG_FIXED_DOMAINS(X)
#undef X
 {"gpuCatalogStrings",sizeof(OfflineCatalogString),false}
};
inline const OfflineCatalogDescriptor& OfflineCatalogInfo(OfflineCatalogDomain domain){const auto id=size_t(domain);if(id>=OfflineCatalogDomainCount)throw std::runtime_error("offline_catalog_domain");return OfflineCatalogDescriptors[id];}
using OfflineCatalogQuality=std::array<uint64_t,4>; // present, valid, Catalog schema, evidence schema
}
#endif
