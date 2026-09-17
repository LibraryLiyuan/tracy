#ifndef TRACY_OFFLINE_CONVERSION_STORAGE_HPP
#define TRACY_OFFLINE_CONVERSION_STORAGE_HPP
#include "../../server/TracyOfflineStorage.hpp"
#include "../../storage/TracyResourceSetStore.hpp"
#include "../../analysis/TracyPagedRecordStore.hpp"
#include "../../storage/TracyMutableRecordStore.hpp"
#include "../../storage/TracyDiskIndex64.hpp"
#include "../../storage/TracyMappedHashIndex64.hpp"
#include "../../storage/TracyResourceIntervals.hpp"
#include "../../storage/TracyBoundedRecordTable.hpp"
#include "../../storage/TracyOrderedForest.hpp"
#include "../../storage/TracyOfflineStringStore.hpp"
#include "../../storage/TracyOfflineCallstackStore.hpp"
#include "../../storage/TracyOfflineSampleStore.hpp"
#include "../../storage/TracyCatalogBatchDirectory.hpp"
#include <vector>
#include <array>
#include <map>
namespace tracy::conversion
{
class OfflineConversionStorage final : public WorkerOfflineStorage
{
public:
    OfflineConversionStorage(std::filesystem::path root,std::string identity,storage::ConversionResources& resources);
    void Check() override;
    void SetFinalizeProgress(std::function<void(const char*,uint64_t,uint64_t,const char*)> callback){m_finalizeProgress=std::move(callback);}
    void ReportFinalizeProgress(const char*,uint64_t,uint64_t,const char*) override;
    OfflinePassTimeBuildStats PreparePassTimes() override;
    OfflinePassTimeBuildStats PassTimeStatistics()const{return m_passTimeStats;}
    std::optional<int64_t> LookupPassTime(uint64_t) override;
    void BeginResourceIntervals() override;
    void AddResourceInterval(const JnGpuCatalogResourceRecordV1&) override;
    void SealResourceIntervals() override;
    uint64_t ResolveResource(uint64_t,int64_t) override;
    void EndResourceIntervals() noexcept override;
    void BeginCatalogBatchDirectory() override;
    void AddCatalogDirectoryGeneration(uint64_t) override;
    void AddCatalogDirectoryBatch(uint64_t,uint64_t,uint8_t) override;
    void SealCatalogBatchDirectory() override;
    bool VisitCatalogDirectoryBatches(uint64_t,uint8_t,const std::function<void(uint64_t)>&) override;
    storage::CatalogBatchDirectory::Stats CatalogBatchDirectoryStatistics()const{return m_batchDirectory?m_batchDirectory->Statistics():m_batchDirectoryStats;}
    std::array<uint64_t,3> ResourceIntervalPeaks()const{return m_intervalPeaks;}
    storage::ResourceIntervals::Stats ResourceIntervalStatistics()const{return m_intervalStats;}
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
    const char* InternString(const char*,size_t,uint32_t&) override;
    bool FindString(const char*,size_t,uint32_t&) override;
    const char* StringAt(uint32_t) override;
    uint64_t StringCount() const override { return m_strings?m_strings->Count():0; }
    void VisitStrings(const std::function<void(uint32_t,const char*,uint64_t)>&) override;
    uint64_t StringUsedBytes()const{return m_strings?m_strings->UsedBytes():0;}
    uint64_t StringReservedBytes()const{return m_strings?m_strings->ReservedBytes():0;}
    std::string StringHash()const;
    bool StringsSealed()const{return m_sealed;}
    const uint64_t* InternCallstack(const uint64_t*,uint32_t,uint32_t&) override;
    uint64_t CallstackCount()const override{return m_callstacks?m_callstacks->Count():1;}
    uint64_t CallstackUsedBytes()const{return m_callstacks?m_callstacks->UsedBytes():0;}
    uint64_t CallstackReservedBytes()const{return m_callstacks?m_callstacks->ReservedBytes():0;}
    std::string CallstackHash()const;
    uint64_t AppendSample(uint64_t,uint8_t,int64_t,uint32_t)override;
    bool FindSample(uint64_t,uint8_t,int64_t,uint64_t&,uint32_t&)override;
    void PutSample(uint64_t,int64_t,uint32_t)override;
    std::optional<int64_t> LastSampleTime(uint64_t,uint8_t)const override;
    uint64_t SampleCount(uint64_t,uint8_t)const override;
    void VisitSamples(uint64_t,uint8_t,const std::function<void(int64_t,uint32_t)>&)override;
    uint64_t TotalSampleCount()const{return m_samples?m_samples->TotalCount():0;}
    uint64_t SampleThreadCount()const{return m_samples?m_samples->ThreadCount():0;}
    bool HasSampleTimeIndex()const{return m_samples&&m_samples->HasTimeIndex();}
    std::string SampleHash(uint64_t,uint8_t);
    uint64_t CreateCpuThread() override;
    uint64_t AppendCpuZone(uint64_t,const OfflineCpuZone&) override;
    void ReadCpuZone(uint64_t,OfflineCpuZone&) override;
    void PutCpuZone(uint64_t,const OfflineCpuZone&) override;
    void SealCpuZones() override;
    void VisitCpuZones(uint64_t,const std::function<void(uint64_t,const OfflineCpuZone&,uint32_t,bool)>&) override;
    uint64_t CpuZoneCount() const override { return m_cpuZones ? m_cpuZones->Count() : 0; }
    uint64_t CpuThreadCount() const override { return m_cpuZones ? m_cpuZones->RootCount() : 0; }
    uint64_t CpuBranchCount() const override { return m_cpuZones ? m_cpuZones->BranchCount() : 0; }
    uint32_t CpuRootChildren(uint64_t root) override { if(!m_cpuZones)throw std::runtime_error("cpu_zone_store_missing");return m_cpuZones->Children(root); }
    uint64_t CpuZoneLogicalBytes() const { return m_cpuZones ? m_cpuZones->LogicalBytes() : 0; }
    void BeginCpuZoneWrite(uint64_t) override;
    void RecordCpuZoneWriteBegin(uint64_t,int64_t,int16_t,uint32_t,uint32_t) override;
    void RecordCpuZoneWriteEnd(int64_t) override;
    void EndCpuZoneWrite() override;
    uint64_t CpuZoneWriteCount() const override { return m_cpuZoneWriteCount; }
    std::string CpuZoneWriteHash() const override;
    uint64_t CreateGpuThread() override;
    uint64_t AppendGpuZone(uint64_t,const OfflineGpuZone&) override;
    void ReadGpuZone(uint64_t,OfflineGpuZone&) override;
    void PutGpuZone(uint64_t,const OfflineGpuZone&) override;
    void SealGpuZones() override;
    void VisitGpuZones(uint64_t,const std::function<void(uint64_t,const OfflineGpuZone&,uint32_t,bool)>&) override;
    uint64_t GpuZoneCount() const override { return m_gpuZones ? m_gpuZones->Count() : 0; }
    uint64_t GpuThreadCount() const override { return m_gpuZones ? m_gpuZones->RootCount() : 0; }
    uint64_t GpuBranchCount() const override { return m_gpuZones ? m_gpuZones->BranchCount() : 0; }
    uint32_t GpuRootChildren(uint64_t root) override { if(!m_gpuZones)throw std::runtime_error("gpu_zone_store_missing");return m_gpuZones->Children(root); }
    uint64_t GpuZoneLogicalBytes() const { return m_gpuZones ? m_gpuZones->LogicalBytes() : 0; }
    void BeginGpuZoneWrite(uint64_t) override;
    void RecordGpuZoneWriteBegin(uint32_t,uint64_t,int64_t,int64_t,int16_t,uint32_t,uint64_t) override;
    void RecordGpuZoneWriteEnd(int64_t,int64_t,uint16_t) override;
    void EndGpuZoneWrite() override;
    uint64_t GpuZoneWriteCount() const override { return m_gpuZoneWriteCount; }
    std::string GpuZoneWriteHash() const override;
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
    analysis::PagedRecordScratch::Stats PagedScratchStatistics() const { return m_pagedScratch->Statistics(); }
    storage::MutableRows::Stats CpuZoneStorageStatistics() const { return m_cpuZones?m_cpuZones->Statistics():storage::MutableRows::Stats{}; }
    storage::MutableRows::Stats GpuZoneStorageStatistics() const { return m_gpuZones?m_gpuZones->Statistics():storage::MutableRows::Stats{}; }
    storage::ResourceSetStore::Stats ResourceSetStatistics() const { return m_definitions?m_definitions->Statistics():m_resourceSetStats; }
private:
    struct AppendTable {std::unique_ptr<analysis::PagedRecordWriter> writer;uint64_t count=0;std::string hash;bool failed=false;};
    std::array<AppendTable,OfflineAppendDomainCount> m_appendTables;
    template<class T> void Visit(const char*,const std::function<void(const T*,uint64_t)>&);
    analysis::PagedRecordOptions Options();
    std::filesystem::path m_root;
    std::string m_identity,m_jobHash,m_useHash;
    storage::ConversionResources& m_resources;
    std::shared_ptr<analysis::PagedRecordScratch> m_pagedScratch;
    analysis::AnalysisWorkspaceReservation m_expandLease;
    analysis::AnalysisWorkspaceReservation m_definitionLease;
    analysis::AnalysisWorkspaceReservation m_sampleHashLease;
    analysis::AnalysisWorkspaceReservation m_sampleHashCacheLease;
    analysis::AnalysisWorkspaceReservation m_zoneWriteHashLease;
    std::array<storage::OfflineSampleRow,4096> m_sampleHashBuffer{};
    std::array<unsigned char,64*1024> m_zoneWriteHashBuffer{};
    size_t m_zoneWriteHashBytes=0;
    uint8_t m_zoneWriteKind=0;
    uint64_t m_zoneWriteExpected=0,m_zoneWriteSeen=0;
    uint64_t m_cpuZoneWriteCount=0,m_gpuZoneWriteCount=0;
    std::unique_ptr<analysis::Sha256Builder> m_zoneWriteHash;
    std::string m_cpuZoneWriteHash,m_gpuZoneWriteHash;
    std::map<std::pair<uint64_t,uint8_t>,std::string> m_sampleHashes;
    std::vector<storage::ResourceSetMember> m_definitionMembers;
    std::vector<storage::ResourceSetMember> m_members;
    std::vector<JnGpuReferenceUseData> m_expanded;
    std::unique_ptr<analysis::PagedRecordWriter> m_jobs,m_uses;
    std::unique_ptr<analysis::PagedRecordWriter> m_relations;
    uint64_t m_relationCount=0;
    std::string m_relationHash;
    bool m_relationFailed=false;
    std::unique_ptr<storage::ResourceSetStore> m_definitions;
    storage::ResourceSetStore::Stats m_resourceSetStats{};
    std::unique_ptr<storage::MutableRecordStore> m_zoneExtras;
    std::unique_ptr<storage::OfflineStringStore> m_strings;
    std::unique_ptr<storage::OfflineCallstackStore> m_callstacks;
    std::unique_ptr<storage::OfflineSampleStore> m_samples;
    std::unique_ptr<storage::OrderedForest> m_cpuZones;
    bool m_cpuZonesSealed=false;
    std::unique_ptr<storage::OrderedForest> m_gpuZones;
    bool m_gpuZonesSealed=false;
    std::unique_ptr<storage::MappedHashIndex64> m_passTimes;
    OfflinePassTimeBuildStats m_passTimeStats{};
    bool m_passTimesPrepared=false;
    std::unique_ptr<storage::ResourceIntervals> m_resourceIntervals;
    std::array<uint64_t,3> m_intervalPeaks{};
    storage::ResourceIntervals::Stats m_intervalStats{};
    std::unique_ptr<storage::CatalogBatchDirectory> m_batchDirectory;
    storage::CatalogBatchDirectory::Stats m_batchDirectoryStats{};
    std::array<std::unique_ptr<storage::BoundedRecordTable>,OfflineCatalogDomainCount> m_catalogTables;
    std::array<std::string,OfflineCatalogDomainCount> m_catalogHashes;
    OfflineCatalogQuality m_catalogQuality{};
    bool m_catalogSealed=false;
    std::unique_ptr<storage::DiskIndex64> m_descriptorHeaps;
    uint64_t m_nextDescriptorHeap=1;
    std::string m_zoneHash;
    uint64_t m_jobCount=0,m_useCount=0,m_definitionCount=0;
    bool m_sealed=false,m_reclaimed=false;
    std::function<void(const char*,uint64_t,uint64_t,const char*)> m_finalizeProgress;
    void AppendZoneWriteHash(const void*,size_t);
    std::string FinishZoneWriteHash(uint8_t);
};
}
#endif
