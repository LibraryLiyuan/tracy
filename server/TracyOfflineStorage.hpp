#ifndef TRACY_OFFLINE_STORAGE_HPP
#define TRACY_OFFLINE_STORAGE_HPP
#include "TracyJnData.hpp"
#include <functional>
namespace tracy
{
class WorkerOfflineStorage
{
public:
    virtual ~WorkerOfflineStorage()=default;
    virtual uint64_t ZoneExtraCount() const=0;
    virtual uint32_t AppendZoneExtra(const void*)=0;
    virtual void ReadZoneExtra(uint32_t,void*)=0;
    virtual void PutZoneExtra(uint32_t,const void*)=0;
    virtual void VisitZoneExtras(const std::function<void(const void*,uint64_t)>&)=0;
    virtual void Check()=0;
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
