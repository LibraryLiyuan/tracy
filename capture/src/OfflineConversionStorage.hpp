#ifndef TRACY_OFFLINE_CONVERSION_STORAGE_HPP
#define TRACY_OFFLINE_CONVERSION_STORAGE_HPP
#include "../../server/TracyOfflineStorage.hpp"
#include "../../storage/TracyResourceSetStore.hpp"
#include "../../analysis/TracyPagedRecordStore.hpp"
#include "../../storage/TracyMutableRecordStore.hpp"
#include "../../storage/TracyDiskIndex64.hpp"
#include "../../storage/TracyResourceIntervals.hpp"
#include "../../storage/TracyBoundedRecordTable.hpp"
#include <vector>
#include <array>
namespace tracy::conversion
{
class OfflineConversionStorage final : public WorkerOfflineStorage
{
public:
    OfflineConversionStorage(std::filesystem::path root,std::string identity,storage::ConversionResources& resources);
    void Check() override;
    std::optional<int64_t> LookupPassTime(uint64_t) override;
    void BeginResourceIntervals() override;
    void AddResourceInterval(const JnGpuCatalogResourceRecordV1&) override;
    void SealResourceIntervals() override;
    uint64_t ResolveResource(uint64_t,int64_t) override;
    void EndResourceIntervals() noexcept override;
    std::array<uint64_t,3> ResourceIntervalPeaks()const{return m_intervalPeaks;}
    uint64_t AppendCatalog(OfflineCatalogDomain,const void*,uint32_t) override;
    uint64_t CatalogCount(OfflineCatalogDomain) const override;
    void ReadCatalog(OfflineCatalogDomain,uint64_t,void*,uint32_t) override;
    void PutCatalog(OfflineCatalogDomain,uint64_t,const void*,uint32_t) override;
    void SealCatalog(OfflineCatalogQuality) override;
    void VisitCatalog(OfflineCatalogDomain,const std::function<void(const void*,uint64_t)>&) override;
    std::string CatalogHash(OfflineCatalogDomain)const;
    OfflineCatalogQuality CatalogQuality()const;
    bool CatalogSealed()const{return m_catalogSealed;}
    uint64_t DescriptorHeapCount()const{return m_descriptorHeaps?m_descriptorHeaps->Count():0;}
    uint64_t AnonymizeDescriptor(uint64_t) override;
    void AppendRecord(OfflineAppendDomain,const void*,uint32_t) override;
    uint64_t RecordCount(OfflineAppendDomain) const override;
    void VisitRecords(OfflineAppendDomain,const std::function<void(const void*,uint64_t)>&) override;
    std::string RecordHash(OfflineAppendDomain) const;
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
    void AppendRelation(const JnRelationData&) override;
    uint64_t RelationCount() const override { return m_relationCount; }
    void VisitRelations(const std::function<void(const JnRelationData*,uint64_t)>&) override;
    std::string RelationHash() const;
    const char* RelationState() const {return m_relationFailed?"failed":m_sealed?"sealed":m_relationCount?"appending":"empty";}
    uint64_t ReferenceUseCount() const override { return m_useCount; }
    uint64_t JobStageCount() const override { return m_jobCount; }
    uint64_t DefinitionCount() const { return m_definitionCount; }
    void VisitReferenceUses(const std::function<void(const JnGpuReferenceUseData*,uint64_t)>&) override;
    void VisitJobStages(const std::function<void(const JnJobStageData*,uint64_t)>&) override;
    void Seal();
    std::string JobStageHash() const;
    std::string ReferenceUseHash() const;
private:
    struct AppendTable {std::unique_ptr<analysis::PagedRecordWriter> writer;uint64_t count=0;std::string hash;bool failed=false;};
    std::array<AppendTable,OfflineAppendDomainCount> m_appendTables;
    template<class T> void Visit(const char*,const std::function<void(const T*,uint64_t)>&);
    analysis::PagedRecordOptions Options();
    std::filesystem::path m_root;
    std::string m_identity,m_jobHash,m_useHash;
    storage::ConversionResources& m_resources;
    analysis::AnalysisWorkspaceReservation m_expandLease;
    std::vector<storage::ResourceSetMember> m_members;
    std::vector<JnGpuReferenceUseData> m_expanded;
    std::unique_ptr<analysis::PagedRecordWriter> m_jobs,m_uses;
    std::unique_ptr<analysis::PagedRecordWriter> m_relations;
    uint64_t m_relationCount=0;
    std::string m_relationHash;
    bool m_relationFailed=false;
    std::unique_ptr<storage::ResourceSetStore> m_definitions;
    std::unique_ptr<storage::MutableRecordStore> m_zoneExtras;
    std::unique_ptr<storage::DiskIndex64> m_passTimes;
    std::unique_ptr<storage::ResourceIntervals> m_resourceIntervals;
    std::array<uint64_t,3> m_intervalPeaks{};
    std::array<std::unique_ptr<storage::BoundedRecordTable>,OfflineCatalogDomainCount> m_catalogTables;
    std::array<std::string,OfflineCatalogDomainCount> m_catalogHashes;
    OfflineCatalogQuality m_catalogQuality{};
    bool m_catalogSealed=false;
    std::unique_ptr<storage::DiskIndex64> m_descriptorHeaps;
    uint64_t m_nextDescriptorHeap=1;
    std::string m_zoneHash;
    uint64_t m_jobCount=0,m_useCount=0,m_definitionCount=0;
    bool m_sealed=false,m_reclaimed=false;
};
}
#endif
