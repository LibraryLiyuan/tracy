#ifndef TRACY_OFFLINE_STORAGE_HPP
#define TRACY_OFFLINE_STORAGE_HPP
#include "TracyJnData.hpp"
#include "TracyOfflineDomains.hpp"
#include "TracyOfflineCatalog.hpp"
#include <functional>
#include <optional>
namespace tracy
{
#pragma pack(push,1)
struct OfflineCpuZone
{
    int64_t start;
    int64_t end;
    int16_t sourceLocation;
    uint32_t extra;
};
#pragma pack(pop)
static_assert( sizeof( OfflineCpuZone ) == 22 );
#pragma pack(push,1)
struct OfflineGpuZone
{
    int64_t cpuStart;
    int64_t cpuEnd;
    int64_t gpuStart;
    int64_t gpuEnd;
    int16_t sourceLocation;
    uint32_t callstack;
    uint16_t thread;
    uint16_t queryId;
};
#pragma pack(pop)
static_assert( sizeof( OfflineGpuZone ) == 42 );

class WorkerOfflineStorage
{
public:
    virtual ~WorkerOfflineStorage()=default;
    virtual void AppendRecord(OfflineAppendDomain,const void*,uint32_t)=0;
    virtual uint64_t RecordCount(OfflineAppendDomain) const=0;
    virtual void VisitRecords(OfflineAppendDomain,const std::function<void(const void*,uint64_t)>&)=0;
    virtual uint64_t ZoneExtraCount() const=0;
    virtual uint32_t AppendZoneExtra(const void*)=0;
    virtual void ReadZoneExtra(uint32_t,void*)=0;
    virtual void PutZoneExtra(uint32_t,const void*)=0;
    virtual void VisitZoneExtras(const std::function<void(const void*,uint64_t)>&)=0;
    virtual const char* InternString(const char*,size_t,uint32_t&)=0;
    virtual bool FindString(const char*,size_t,uint32_t&)=0;
    virtual const char* StringAt(uint32_t)=0;
    virtual uint64_t StringCount() const=0;
    virtual void VisitStrings(const std::function<void(uint32_t,const char*,uint64_t)>&)=0;
    virtual const uint64_t* InternCallstack(const uint64_t*,uint32_t,uint32_t&)=0;
    virtual uint64_t CallstackCount() const=0;
    virtual uint64_t CreateCpuThread()=0;
    virtual uint64_t AppendCpuZone(uint64_t,const OfflineCpuZone&)=0;
    virtual void ReadCpuZone(uint64_t,OfflineCpuZone&)=0;
    virtual void PutCpuZone(uint64_t,const OfflineCpuZone&)=0;
    virtual void SealCpuZones()=0;
    virtual void VisitCpuZones(uint64_t,const std::function<void(uint64_t,const OfflineCpuZone&,uint32_t,bool)>&)=0;
    virtual uint64_t CpuZoneCount() const=0;
    virtual uint64_t CpuThreadCount() const=0;
    virtual uint64_t CpuBranchCount() const=0;
    virtual uint32_t CpuRootChildren(uint64_t)=0;
    virtual uint64_t CreateGpuThread()=0;
    virtual uint64_t AppendGpuZone(uint64_t,const OfflineGpuZone&)=0;
    virtual void ReadGpuZone(uint64_t,OfflineGpuZone&)=0;
    virtual void PutGpuZone(uint64_t,const OfflineGpuZone&)=0;
    virtual void SealGpuZones()=0;
    virtual void VisitGpuZones(uint64_t,const std::function<void(uint64_t,const OfflineGpuZone&,uint32_t,bool)>&)=0;
    virtual uint64_t GpuZoneCount() const=0;
    virtual uint64_t GpuThreadCount() const=0;
    virtual uint64_t GpuBranchCount() const=0;
    virtual uint32_t GpuRootChildren(uint64_t)=0;
    virtual void Check()=0;
    virtual std::optional<int64_t> LookupPassTime(uint64_t)=0;
    virtual void BeginResourceIntervals()=0;
    virtual void AddResourceInterval(const JnGpuCatalogResourceRecordV1&)=0;
    virtual void SealResourceIntervals()=0;
    virtual uint64_t ResolveResource(uint64_t,int64_t)=0;
    virtual void EndResourceIntervals() noexcept=0;
    virtual uint64_t AppendCatalog(OfflineCatalogDomain,const void*,uint32_t)=0;
    virtual uint64_t CatalogCount(OfflineCatalogDomain) const=0;
    virtual void ReadCatalog(OfflineCatalogDomain,uint64_t,void*,uint32_t)=0;
    virtual void PutCatalog(OfflineCatalogDomain,uint64_t,const void*,uint32_t)=0;
    virtual void SealCatalog(OfflineCatalogQuality)=0;
    virtual void VisitCatalog(OfflineCatalogDomain,const std::function<void(const void*,uint64_t)>&)=0;
    virtual uint64_t AnonymizeDescriptor(uint64_t)=0;
    virtual void Fail(const char*) noexcept=0;
    virtual bool DefineResourceSet(uint32_t,const char*,size_t)=0;
    virtual bool AppendReferenceSet(uint32_t,uint16_t,int64_t,uint64_t,uint64_t,uint8_t,uint8_t)=0;
    virtual void AppendReferenceUse(const JnGpuReferenceUseData&)=0;
    virtual void AppendJobStage(const JnJobStageData&)=0;
    virtual void AppendRelation(const JnRelationData&)=0;
    virtual uint64_t RelationCount() const=0;
    virtual void VisitRelations(const std::function<void(const JnRelationData*,uint64_t)>&)=0;
    virtual uint64_t ReferenceUseCount() const=0;
    virtual uint64_t JobStageCount() const=0;
    virtual void VisitReferenceUses(const std::function<void(const JnGpuReferenceUseData*,uint64_t)>&)=0;
    virtual void VisitJobStages(const std::function<void(const JnJobStageData*,uint64_t)>&)=0;
};
}
#endif
