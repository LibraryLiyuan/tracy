#ifndef TRACY_OFFLINE_CONVERSION_STORAGE_HPP
#define TRACY_OFFLINE_CONVERSION_STORAGE_HPP
#include "../../server/TracyOfflineStorage.hpp"
#include "../../storage/TracyResourceSetStore.hpp"
#include "../../analysis/TracyPagedRecordStore.hpp"
#include "../../storage/TracyMutableRecordStore.hpp"
#include <vector>
namespace tracy::conversion
{
class OfflineConversionStorage final : public WorkerOfflineStorage
{
public:
    OfflineConversionStorage(std::filesystem::path root,std::string identity,storage::ConversionResources& resources);
    void Check() override;
    uint64_t ZoneExtraCount() const override { return m_zoneExtras?m_zoneExtras->Count():1; }
    uint32_t AppendZoneExtra(const void*) override;
    void ReadZoneExtra(uint32_t,void*) override;
    void PutZoneExtra(uint32_t,const void*) override;
    void VisitZoneExtras(const std::function<void(const void*,uint64_t)>&) override;
    std::string ZoneExtraHash() const { if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");return m_zoneHash; }
    void Fail(const char* reason) noexcept override { m_resources.Fail(reason); }
    bool DefineResourceSet(uint32_t,const char*,size_t) override;
    bool AppendReferenceSet(uint32_t,uint16_t,int64_t,uint64_t,uint64_t,uint8_t,uint8_t) override;
    void AppendReferenceUse(const JnGpuReferenceUseData&) override;
    void AppendJobStage(const JnJobStageData&) override;
    uint64_t ReferenceUseCount() const override { return m_useCount; }
    uint64_t JobStageCount() const override { return m_jobCount; }
    uint64_t DefinitionCount() const { return m_definitionCount; }
    void VisitReferenceUses(const std::function<void(const JnGpuReferenceUseData*,uint64_t)>&) override;
    void VisitJobStages(const std::function<void(const JnJobStageData*,uint64_t)>&) override;
    void Seal();
    std::string JobStageHash() const;
    std::string ReferenceUseHash() const;
private:
    template<class T> void Visit(const char*,const std::function<void(const T*,uint64_t)>&);
    analysis::PagedRecordOptions Options();
    std::filesystem::path m_root;
    std::string m_identity,m_jobHash,m_useHash;
    storage::ConversionResources& m_resources;
    analysis::AnalysisWorkspaceReservation m_expandLease;
    std::vector<storage::ResourceSetMember> m_members;
    std::vector<JnGpuReferenceUseData> m_expanded;
    std::unique_ptr<analysis::PagedRecordWriter> m_jobs,m_uses;
    std::unique_ptr<storage::ResourceSetStore> m_definitions;
    std::unique_ptr<storage::MutableRecordStore> m_zoneExtras;
    std::string m_zoneHash;
    uint64_t m_jobCount=0,m_useCount=0,m_definitionCount=0;
    bool m_sealed=false,m_reclaimed=false;
};
}
#endif
