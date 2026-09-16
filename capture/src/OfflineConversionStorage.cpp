#include "OfflineConversionStorage.hpp"
#include "../../analysis/TracyHash.hpp"
#include "../../analysis/TracyAnalysisIoPath.hpp"
#include "../../storage/TracyZoneExtraCodec.hpp"
#include "../../public/common/TracyQueue.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <bit>
namespace tracy::conversion
{
analysis::PagedRecordOptions OfflineConversionStorage::Options()
{
    analysis::PagedRecordOptions o;o.workspace=m_resources.Workspace();
    o.beforeMemoryAllocation=[this](uint64_t bytes){m_resources.Check(bytes);};
    o.cancelled=[this]{m_resources.CheckFast();return false;};
    o.beforeFileGrowth=[this](const auto& p,uint64_t n){m_resources.BeforeFileGrowth(p,n);};
    o.renameFile=[this](const auto& a,const auto& b){m_resources.RenameOwned(a,b);};return o;
}
OfflineConversionStorage::OfflineConversionStorage(std::filesystem::path root,std::string identity,storage::ConversionResources& resources)
    :m_root(std::move(root)),m_identity(std::move(identity)),m_resources(resources),
     m_expandLease(resources.Workspace(),65536),m_members(1024),m_expanded(1024)
{
    if(std::filesystem::exists(analysis::AnalysisIoPath(m_root)))throw std::runtime_error("offline_store_already_exists");
    resources.BeforeFileGrowth(m_root,0);std::filesystem::create_directories(analysis::AnalysisIoPath(m_root));
}
void OfflineConversionStorage::Check()
{
    m_resources.CheckFast();
    if(m_resources.SoftTargetExceeded() && !m_reclaimed) {if(m_definitions)m_definitions->ReduceCaches();if(m_zoneExtras)m_zoneExtras->ReduceCache();if(m_strings)m_strings->ReduceCache();if(m_callstacks)m_callstacks->ReduceCache();if(m_samples)m_samples->ReduceCache();if(m_cpuZones)m_cpuZones->ReduceCache();if(m_gpuZones)m_gpuZones->ReduceCache();if(m_descriptorHeaps)m_descriptorHeaps->ReduceCache();for(auto& table:m_catalogTables)if(table)table->ReduceCache();m_reclaimed=true;}
    if(!m_resources.SoftTargetExceeded())m_reclaimed=false;
}
bool OfflineConversionStorage::DefineResourceSet(uint32_t id,const char* data,size_t bytes)
{
    Check();if(m_sealed)throw std::runtime_error("offline_store_closed");
    if(!data||!bytes||bytes%sizeof(storage::ResourceSetMember)||bytes/sizeof(storage::ResourceSetMember)>65535)return false;
    const auto count=bytes/sizeof(storage::ResourceSetMember);
    analysis::AnalysisWorkspaceReservation memory(m_resources.Workspace(),bytes+256);
    std::vector<storage::ResourceSetMember> members(count);std::memcpy(members.data(),data,bytes);
    if(!m_definitions)m_definitions=std::make_unique<storage::ResourceSetStore>(m_root/"sets",m_identity,m_resources);
    const auto inserted=m_definitions->Define(id,members);if(inserted)++m_definitionCount;return inserted;
}
bool OfflineConversionStorage::AppendReferenceSet(uint32_t id,uint16_t count,int64_t time,uint64_t pass,uint64_t thread,uint8_t flags,uint8_t encoding)
{
    Check();if(m_sealed)throw std::runtime_error("offline_store_closed");
    if(!m_definitions)return false;
    const auto definition=m_definitions->Lookup(id);if(encoding!=2||!definition||definition->count!=count)return false;
    if(!m_uses)m_uses=std::make_unique<analysis::PagedRecordWriter>(m_root/"uses",m_identity,uint32_t(sizeof(JnGpuReferenceUseData)),Options());
    uint32_t offset=0;
    while(offset<count) {
        Check();const auto take=std::min<uint32_t>(uint32_t(m_members.size()),count-offset);
        m_definitions->Read(*definition,offset,std::span(m_members.data(),take));
        for(uint32_t i=0;i<take;++i)m_expanded[i]={time,pass,m_members[i].resourceId,thread,m_members[i].usageMask,id,flags,encoding};
        m_uses->Append(m_expanded.data(),take);m_useCount+=take;offset+=take;
    }
    return true;
}
void OfflineConversionStorage::AppendReferenceUse(const JnGpuReferenceUseData& row)
{Check();if(m_sealed)throw std::runtime_error("offline_store_closed");if(!m_uses)m_uses=std::make_unique<analysis::PagedRecordWriter>(m_root/"uses",m_identity,uint32_t(sizeof(JnGpuReferenceUseData)),Options());m_uses->Append(&row,1);++m_useCount;}
void OfflineConversionStorage::AppendJobStage(const JnJobStageData& row)
{Check();if(m_sealed)throw std::runtime_error("offline_store_closed");if(!m_jobs)m_jobs=std::make_unique<analysis::PagedRecordWriter>(m_root/"jobs",m_identity,uint32_t(sizeof(JnJobStageData)),Options());m_jobs->Append(&row,1);++m_jobCount;}
void OfflineConversionStorage::Seal()
{
    Check();if(m_sealed)throw std::runtime_error("offline_store_closed");
    SealCpuZones();
    SealGpuZones();
    if(m_strings)m_strings->Seal();
    if(m_callstacks)m_callstacks->Seal();
    if(m_samples)m_samples->Seal();
    for(auto& table:m_catalogTables)if(table)table->FreezeAppend();
    m_jobHash=analysis::Sha256Builder().FinalHex();m_useHash=m_jobHash;
    m_relationHash=m_jobHash;
    for(auto& table:m_appendTables){
        if(table.failed)throw std::runtime_error("offline_append_failed");table.hash=m_jobHash;
        if(table.writer){try{if(table.count!=table.writer->Count())throw std::runtime_error("offline_append_count");table.writer->Commit();table.hash=table.writer->ContentHash();table.writer.reset();}catch(...){table.failed=true;throw;}}
    }
    if(m_relationFailed)throw std::runtime_error("relation_store_failed");
    if(m_relations){
        try {
            if(m_relations->Count()!=m_relationCount)throw std::runtime_error("relation_store_count");
            m_relations->Commit();m_relationHash=m_relations->ContentHash();
        }catch(...){m_relationFailed=true;throw;}
    }
    if(m_jobs){m_jobs->Commit();m_jobHash=m_jobs->ContentHash();}
    if(m_uses){m_uses->Commit();m_useHash=m_uses->ContentHash();}
    if(m_definitions)m_definitions->Seal();
    analysis::Sha256Builder zoneHash;
    if(m_zoneExtras){
        m_zoneExtras->Seal();
        analysis::AnalysisWorkspaceReservation encodedMemory(m_resources.Workspace(),storage::MutableRecordStore::Rows*storage::ZoneExtraWireBytes);
        std::array<uint8_t,storage::MutableRecordStore::Rows*storage::ZoneExtraWireBytes> encoded{};
        m_zoneExtras->Visit([&](const auto* rows,uint64_t n){for(uint64_t i=0;i<n;++i)storage::EncodeZoneExtra32(rows[i],encoded.data()+i*storage::ZoneExtraWireBytes);zoneHash.Update(encoded.data(),size_t(n)*storage::ZoneExtraWireBytes);});
    }
    else {std::array<uint8_t,storage::ZoneExtraWireBytes> zero{};zoneHash.Update(zero.data(),zero.size());}
    m_zoneHash=zoneHash.FinalHex();
    m_jobs.reset();m_uses.reset();m_definitions.reset();m_members.clear();m_members.shrink_to_fit();m_expanded.clear();m_expanded.shrink_to_fit();m_expandLease.Resize(0);
    m_sealed=true;
    m_relations.reset();
}

void OfflineConversionStorage::AppendRelation(const JnRelationData& row)
{
    static_assert(sizeof(JnRelationData)==37);
    Check();if(m_sealed||m_relationFailed)throw std::runtime_error("relation_store_closed");
    try {
        if(!m_relations)m_relations=std::make_unique<analysis::PagedRecordWriter>(m_root/"relations",m_identity,uint32_t(sizeof(row)),Options());
        m_relations->Append(&row,1);m_relationCount=m_relations->Count();
    }catch(...){m_relationFailed=true;throw;}
}
void OfflineConversionStorage::ReportFinalizeProgress(const char* phase,uint64_t completed,uint64_t total,const char* unit){if(m_finalizeProgress)m_finalizeProgress(phase,completed,total,unit);}
OfflinePassTimeBuildStats OfflineConversionStorage::PreparePassTimes()
{
    Check();if(!m_sealed)throw std::runtime_error("pass_index_before_seal");if(m_passTimesPrepared)return m_passTimeStats;
    try {
        const auto started=std::chrono::steady_clock::now();auto index=std::make_unique<storage::MappedHashIndex64>(m_root/"pass-times.index",m_resources);const auto total=RecordCount(OfflineAppendDomain::GpuReferencePass)+RecordCount(OfflineAppendDomain::GfxEntity)+RecordCount(OfflineAppendDomain::GfxLink);ReportFinalizeProgress("pass_time_build",0,total,"source_rows");
        const auto update=[&]{if((m_passTimeStats.sourceRows&0xFFFF)==0||m_passTimeStats.sourceRows==total)ReportFinalizeProgress("pass_time_build",m_passTimeStats.sourceRows,total,"source_rows");};
        VisitRecords(OfflineAppendDomain::GpuReferencePass,[&](const void* p,uint64_t n){m_passTimeStats.sourceRows+=n;auto rows=static_cast<const JnGpuReferencePassData*>(p);for(uint64_t i=0;i<n;++i)if(rows[i].passId){++m_passTimeStats.candidateRows;++m_passTimeStats.upserts;index->Upsert(rows[i].passId,std::bit_cast<uint64_t>(rows[i].time));}update();});
        VisitRecords(OfflineAppendDomain::GfxEntity,[&](const void* p,uint64_t n){m_passTimeStats.sourceRows+=n;auto rows=static_cast<const JnGfxEntityData*>(p);for(uint64_t i=0;i<n;++i)if(rows[i].entityId&&rows[i].kind==uint8_t(JnGfxEntityKind::ExplicitGpuPass)){++m_passTimeStats.candidateRows;++m_passTimeStats.upserts;index->Upsert(rows[i].entityId,std::bit_cast<uint64_t>(rows[i].time));}update();});
        VisitRecords(OfflineAppendDomain::GfxLink,[&](const void* p,uint64_t n){m_passTimeStats.sourceRows+=n;auto rows=static_cast<const JnGfxLinkData*>(p);for(uint64_t i=0;i<n;++i)if(rows[i].sourceId&&rows[i].relation==uint8_t(JnGfxRelation::RangeEvidenceComplete)){++m_passTimeStats.candidateRows;++m_passTimeStats.upserts;index->Upsert(rows[i].sourceId,std::bit_cast<uint64_t>(rows[i].time));}update();});
        m_passTimeStats.uniqueKeys=index->Count();m_passTimeStats.buildNanoseconds=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count());
        ReportFinalizeProgress("pass_time_seal",0,index->Count(),"keys");const auto sealStarted=std::chrono::steady_clock::now();index->Seal();m_passTimeStats.sealNanoseconds=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-sealStarted).count());ReportFinalizeProgress("pass_time_seal",index->Count(),index->Count(),"keys");
        m_passTimes=std::move(index);m_passTimesPrepared=true;m_passTimeStats.buildCount=1;return m_passTimeStats;
    }catch(...){m_resources.Fail("pass_index_build_failed");throw;}
}
std::optional<int64_t> OfflineConversionStorage::LookupPassTime(uint64_t id)
{
    Check();if(!m_sealed||!m_passTimesPrepared)throw std::runtime_error("pass_index_not_prepared");if(!id||!m_passTimes)return {};
    const auto value=m_passTimes->Find(id);return value?std::optional<int64_t>(std::bit_cast<int64_t>(*value)):std::nullopt;
}
void OfflineConversionStorage::BeginResourceIntervals(){Check();if(!m_sealed||m_resourceIntervals)throw std::runtime_error("resource_interval_begin_state");m_resourceIntervals=std::make_unique<storage::ResourceIntervals>(m_root/"resource-intervals",m_resources,m_identity);}
void OfflineConversionStorage::AddResourceInterval(const JnGpuCatalogResourceRecordV1& row){Check();if(!m_resourceIntervals)throw std::runtime_error("resource_interval_missing");m_resourceIntervals->Add(row);}
void OfflineConversionStorage::SealResourceIntervals(){Check();if(!m_resourceIntervals)throw std::runtime_error("resource_interval_missing");m_resourceIntervals->Seal();}
uint64_t OfflineConversionStorage::ResolveResource(uint64_t pointer,int64_t time){Check();if(!m_resourceIntervals)throw std::runtime_error("resource_interval_missing");return m_resourceIntervals->Resolve(pointer,time);}
void OfflineConversionStorage::EndResourceIntervals() noexcept {if(m_resourceIntervals){m_intervalPeaks[0]=std::max(m_intervalPeaks[0],m_resourceIntervals->Count());m_intervalPeaks[1]=std::max(m_intervalPeaks[1],m_resourceIntervals->PointerCount());m_intervalPeaks[2]=std::max(m_intervalPeaks[2],m_resourceIntervals->LogicalBytes());const auto stats=m_resourceIntervals->Statistics();m_intervalStats.adds+=stats.adds;m_intervalStats.resolves+=stats.resolves;m_intervalStats.intervalNodeVisits+=stats.intervalNodeVisits;m_intervalStats.maxNodesPerResolve=std::max(m_intervalStats.maxNodesPerResolve,stats.maxNodesPerResolve);m_resourceIntervals.reset();}}
void OfflineConversionStorage::BeginCatalogBatchDirectory(){Check();if(!m_sealed||m_catalogSealed||m_batchDirectory)throw std::runtime_error("catalog_batch_directory_begin_state");m_batchDirectory=std::make_unique<storage::CatalogBatchDirectory>(m_root/"catalog-batches",m_identity,m_resources);}
void OfflineConversionStorage::AddCatalogDirectoryGeneration(uint64_t generation){Check();if(!m_batchDirectory)throw std::runtime_error("catalog_batch_directory_missing");m_batchDirectory->AddGeneration(generation);}
void OfflineConversionStorage::AddCatalogDirectoryBatch(uint64_t batch,uint64_t generation,uint8_t kind){Check();if(!m_batchDirectory)throw std::runtime_error("catalog_batch_directory_missing");m_batchDirectory->AddBatch(batch,generation,kind);}
void OfflineConversionStorage::SealCatalogBatchDirectory(){Check();if(!m_batchDirectory)throw std::runtime_error("catalog_batch_directory_missing");m_batchDirectory->Seal();m_batchDirectoryStats=m_batchDirectory->Statistics();}
bool OfflineConversionStorage::VisitCatalogDirectoryBatches(uint64_t generation,uint8_t kind,const std::function<void(uint64_t)>& visitor){Check();if(!m_batchDirectory)return false;m_batchDirectory->Visit(generation,kind,visitor);return true;}
uint64_t OfflineConversionStorage::AppendCatalog(OfflineCatalogDomain domain,const void* row,uint32_t width)
{
    Check();const auto& info=OfflineCatalogInfo(domain);if(m_sealed||!row||width!=info.width)throw std::runtime_error("offline_catalog_append_state");
    auto& table=m_catalogTables[size_t(domain)];if(!table)table=std::make_unique<storage::BoundedRecordTable>(m_root/(std::string(info.name)+".catalog"),m_identity,width,info.mutableRows,m_resources);return table->Append(row);
}
uint64_t OfflineConversionStorage::CatalogCount(OfflineCatalogDomain domain)const{OfflineCatalogInfo(domain);const auto& table=m_catalogTables[size_t(domain)];return table?table->Count():0;}
void OfflineConversionStorage::ReadCatalog(OfflineCatalogDomain domain,uint64_t id,void* row,uint32_t width){Check();if(width!=OfflineCatalogInfo(domain).width||!row||id>=CatalogCount(domain))throw std::runtime_error("offline_catalog_read_range");m_catalogTables[size_t(domain)]->Read(id,row);}
void OfflineConversionStorage::PutCatalog(OfflineCatalogDomain domain,uint64_t id,const void* row,uint32_t width){Check();if(m_catalogSealed||width!=OfflineCatalogInfo(domain).width||!row||id>=CatalogCount(domain))throw std::runtime_error("offline_catalog_update_range");m_catalogTables[size_t(domain)]->Put(id,row);}
void OfflineConversionStorage::SealCatalog(OfflineCatalogQuality quality)
{
    Check();if(!m_sealed||m_catalogSealed)throw std::runtime_error("offline_catalog_seal_state");
    if(m_descriptorHeaps)m_descriptorHeaps->Seal();
    if(quality[0]>1||quality[1]>1)throw std::runtime_error("offline_catalog_quality");
    if(!quality[0]){if(quality[1])throw std::runtime_error("offline_catalog_presence_mismatch");for(const auto& table:m_catalogTables)if(table&&table->Count())throw std::runtime_error("offline_catalog_presence_mismatch");}
    ReportFinalizeProgress("catalog_seal",0,OfflineCatalogDomainCount,"domains");for(size_t id=0;id<OfflineCatalogDomainCount;++id){analysis::Sha256Builder hash;auto& table=m_catalogTables[id];
        if(table){table->Seal();table->Visit([&](const void* bytes,uint64_t count){
            if(id!=size_t(OfflineCatalogDomain::CatalogString)){hash.Update(bytes,size_t(count)*OfflineCatalogDescriptors[id].width);return;}
            for(uint64_t i=0;i<count;++i){OfflineCatalogString row;std::memcpy(&row,static_cast<const unsigned char*>(bytes)+i*sizeof(row),sizeof(row));if(row.header.byteLength>256)throw std::runtime_error("offline_catalog_string_length");hash.Update(&row.generation,sizeof(row.generation));hash.Update(&row.header,sizeof(row.header));hash.Update(row.bytes.data(),row.header.byteLength);}
        });}
        m_catalogHashes[id]=hash.FinalHex();ReportFinalizeProgress("catalog_seal",id+1,OfflineCatalogDomainCount,"domains");
    }
    m_catalogQuality=quality;if(m_batchDirectory){m_batchDirectoryStats=m_batchDirectory->Statistics();if(!m_batchDirectory->CloseAndRemove())throw std::runtime_error("catalog_batch_directory_cleanup_failed");m_batchDirectory.reset();}if(m_passTimes){if(!m_passTimes->CloseAndRemove())throw std::runtime_error("pass_index_cleanup_failed");m_passTimes.reset();}m_catalogSealed=true;
}
void OfflineConversionStorage::VisitCatalog(OfflineCatalogDomain domain,const std::function<void(const void*,uint64_t)>& visitor){Check();OfflineCatalogInfo(domain);if(!m_catalogSealed)throw std::runtime_error("offline_catalog_not_sealed");auto& table=m_catalogTables[size_t(domain)];if(table)table->Visit(visitor);}
std::string OfflineConversionStorage::CatalogHash(OfflineCatalogDomain domain)const{OfflineCatalogInfo(domain);if(!m_catalogSealed)throw std::runtime_error("offline_catalog_not_sealed");return m_catalogHashes[size_t(domain)];}
OfflineCatalogQuality OfflineConversionStorage::CatalogQuality()const{if(!m_catalogSealed)throw std::runtime_error("offline_catalog_not_sealed");return m_catalogQuality;}
uint64_t OfflineConversionStorage::AnonymizeDescriptor(uint64_t token)
{
    Check();if(!m_sealed||m_catalogSealed)throw std::runtime_error("descriptor_index_state");
    if(!m_descriptorHeaps)m_descriptorHeaps=std::make_unique<storage::DiskIndex64>(m_root/"descriptor-heaps.index",m_identity,m_resources,m_resources.SoftTargetExceeded()?(4ull<<20):(8ull<<20));
    if(const auto existing=m_descriptorHeaps->Find(token))return *existing;
    if(m_nextDescriptorHeap>0x0FFFFFFFFFFFFFFFull)throw std::runtime_error("descriptor_id_overflow");
    const auto id=0xD000000000000000ull|m_nextDescriptorHeap;m_descriptorHeaps->Upsert(token,id);++m_nextDescriptorHeap;return id;
}
void OfflineConversionStorage::AppendRecord(OfflineAppendDomain domain,const void* row,uint32_t width)
{
    Check();const auto& info=OfflineAppendInfo(domain);auto& table=m_appendTables[size_t(domain)];
    if(m_sealed||table.failed||width!=info.width||!row)throw std::runtime_error("offline_append_state_or_width");
    try{if(!table.writer){auto options=Options();options.blockBytes=1024*1024;table.writer=std::make_unique<analysis::PagedRecordWriter>(m_root/info.name,m_identity,width,options);}table.writer->Append(row,1);table.count=table.writer->Count();}
    catch(...){table.failed=true;throw;}
}
uint64_t OfflineConversionStorage::RecordCount(OfflineAppendDomain domain) const
{OfflineAppendInfo(domain);return m_appendTables[size_t(domain)].count;}
std::string OfflineConversionStorage::RecordHash(OfflineAppendDomain domain) const
{OfflineAppendInfo(domain);const auto& table=m_appendTables[size_t(domain)];if(!m_sealed||table.failed)throw std::runtime_error("offline_append_not_sealed");return table.hash;}
void OfflineConversionStorage::VisitRecords(OfflineAppendDomain domain,const std::function<void(const void*,uint64_t)>& visitor)
{
    Check();const auto& info=OfflineAppendInfo(domain);auto& table=m_appendTables[size_t(domain)];
    if(!m_sealed||table.failed)throw std::runtime_error("offline_append_not_sealed");if(!table.count)return;
    auto options=Options();options.blockBytes=1024*1024;analysis::PagedRecordReader reader(m_root/info.name,m_identity,info.width,options);const auto stats=reader.VisitValidated(visitor);
    if(reader.Count()!=table.count||stats.records!=table.count)throw std::runtime_error("offline_append_count");
}
void OfflineConversionStorage::VisitRelations(const std::function<void(const JnRelationData*,uint64_t)>& visitor)
{Check();if(!m_sealed||m_relationFailed)throw std::runtime_error("relation_store_not_sealed");if(m_relationCount)Visit("relations",visitor);}
std::string OfflineConversionStorage::RelationHash() const
{if(!m_sealed||m_relationFailed)throw std::runtime_error("relation_store_not_sealed");return m_relationHash;}
template<class T> void OfflineConversionStorage::Visit(const char* domain,const std::function<void(const T*,uint64_t)>& visitor)
{
    Check();if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");
    analysis::PagedRecordReader reader(m_root/domain,m_identity,uint32_t(sizeof(T)),Options());const auto count=reader.Count();const auto stats=reader.VisitValidated([&](const void* rows,uint64_t n){visitor(static_cast<const T*>(rows),n);});if(stats.records!=count)throw std::runtime_error("offline_store_count");
}
void OfflineConversionStorage::VisitReferenceUses(const std::function<void(const JnGpuReferenceUseData*,uint64_t)>& f){Check();if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");if(m_useCount)Visit("uses",f);}
void OfflineConversionStorage::VisitJobStages(const std::function<void(const JnJobStageData*,uint64_t)>& f){Check();if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");if(m_jobCount)Visit("jobs",f);}
std::string OfflineConversionStorage::JobStageHash() const {if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");return m_jobHash;}
std::string OfflineConversionStorage::ReferenceUseHash() const {if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");return m_useHash;}
uint32_t OfflineConversionStorage::AppendZoneExtra(const void* value){
    Check();if(m_sealed)throw std::runtime_error("offline_store_closed");
    if(!m_zoneExtras){m_zoneExtras=std::make_unique<storage::MutableRecordStore>(m_root/"zone-extras.bin",m_resources);m_zoneExtras->Append({});}
    return m_zoneExtras->Append(storage::DecodeZoneExtra32(value));
}
void OfflineConversionStorage::ReadZoneExtra(uint32_t id,void* value){Check();if(!m_zoneExtras){if(id)throw std::runtime_error("zone_extra_index");std::memset(value,0,storage::ZoneExtraWireBytes);return;}storage::EncodeZoneExtra32(m_zoneExtras->Read(id),value);}
void OfflineConversionStorage::PutZoneExtra(uint32_t id,const void* value){Check();if(m_sealed||!m_zoneExtras)throw std::runtime_error("zone_extra_update_state");m_zoneExtras->Put(id,storage::DecodeZoneExtra32(value));}
void OfflineConversionStorage::VisitZoneExtras(const std::function<void(const void*,uint64_t)>& f){
    Check();if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");
    if(!m_zoneExtras){std::array<uint8_t,storage::ZoneExtraWireBytes> zero{};f(zero.data(),1);return;}
    analysis::AnalysisWorkspaceReservation encodedMemory(m_resources.Workspace(),storage::MutableRecordStore::Rows*storage::ZoneExtraWireBytes);
    std::array<uint8_t,storage::MutableRecordStore::Rows*storage::ZoneExtraWireBytes> encoded{};
    m_zoneExtras->Visit([&](const auto* rows,uint64_t n){for(uint64_t i=0;i<n;++i)storage::EncodeZoneExtra32(rows[i],encoded.data()+i*storage::ZoneExtraWireBytes);f(encoded.data(),n);});
}
const char* OfflineConversionStorage::InternString(const char* value,size_t bytes,uint32_t& id){Check();if(m_sealed)throw std::runtime_error("offline_store_closed");if(!m_strings)m_strings=std::make_unique<storage::OfflineStringStore>(m_root/"strings",m_identity,m_resources);const auto result=m_strings->Intern(value,bytes);id=result.id;return result.data;}
bool OfflineConversionStorage::FindString(const char* value,size_t bytes,uint32_t& id){Check();if(!m_strings)return false;const auto result=m_strings->Find(value,bytes);if(!result)return false;id=result->id;return true;}
const char* OfflineConversionStorage::StringAt(uint32_t id){Check();if(!m_strings)throw std::runtime_error("offline_string_store_missing");return m_strings->Get(id);}
void OfflineConversionStorage::VisitStrings(const std::function<void(uint32_t,const char*,uint64_t)>& visitor){Check();if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");if(m_strings)m_strings->Visit(visitor);}
std::string OfflineConversionStorage::StringHash()const{if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");return m_strings?m_strings->Hash():analysis::Sha256Builder().FinalHex();}
const uint64_t* OfflineConversionStorage::InternCallstack(const uint64_t* frames,uint32_t count,uint32_t& id){Check();if(m_sealed)throw std::runtime_error("offline_store_closed");if(!m_callstacks)m_callstacks=std::make_unique<storage::OfflineCallstackStore>(m_root/"callstacks",m_identity,m_resources);const auto result=m_callstacks->Intern(frames,count);id=result.id;return result.frames;}
std::string OfflineConversionStorage::CallstackHash()const{if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");return m_callstacks?m_callstacks->Hash():analysis::Sha256Builder().FinalHex();}
uint64_t OfflineConversionStorage::AppendSample(uint64_t thread,uint8_t kind,int64_t time,uint32_t callstack){Check();if(m_sealed)throw std::runtime_error("offline_store_closed");if(!m_samples)m_samples=std::make_unique<storage::OfflineSampleStore>(m_root/"samples",m_identity,m_resources);return m_samples->Append(thread,kind,{time,callstack});}
bool OfflineConversionStorage::FindSample(uint64_t thread,uint8_t kind,int64_t time,uint64_t& id,uint32_t& callstack){Check();if(m_sealed||!m_samples)return false;const auto value=m_samples->Find(thread,kind,time);if(!value)return false;id=value->first;callstack=value->second.callstack;return true;}
void OfflineConversionStorage::PutSample(uint64_t id,int64_t time,uint32_t callstack){Check();if(m_sealed||!m_samples)throw std::runtime_error("offline_sample_update_state");m_samples->Put(id,{time,callstack});}
std::optional<int64_t> OfflineConversionStorage::LastSampleTime(uint64_t thread,uint8_t kind)const{return m_samples?m_samples->LastTime(thread,kind):std::nullopt;}
uint64_t OfflineConversionStorage::SampleCount(uint64_t thread,uint8_t kind)const{return m_samples?m_samples->Count(thread,kind):0;}
void OfflineConversionStorage::VisitSamples(uint64_t thread,uint8_t kind,const std::function<void(int64_t,uint32_t)>& visitor){Check();if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");if(m_samples)m_samples->Visit(thread,kind,[&](const storage::OfflineSampleRow& row){visitor(row.time,row.callstack);});}
std::string OfflineConversionStorage::SampleHash(uint64_t thread,uint8_t kind){analysis::Sha256Builder hash;VisitSamples(thread,kind,[&](int64_t time,uint32_t callstack){storage::OfflineSampleRow row{time,callstack};hash.Update(&row,sizeof(row));});return hash.FinalHex();}
uint64_t OfflineConversionStorage::CreateCpuThread(){Check();if(m_sealed||m_cpuZonesSealed)throw std::runtime_error("cpu_zone_store_closed");if(!m_cpuZones)m_cpuZones=std::make_unique<storage::OrderedForest>(m_root/"cpu-zones.bin",sizeof(OfflineCpuZone),m_resources);return m_cpuZones->CreateRoot();}
uint64_t OfflineConversionStorage::AppendCpuZone(uint64_t parent,const OfflineCpuZone& row){Check();if(m_sealed||m_cpuZonesSealed||!m_cpuZones)throw std::runtime_error("cpu_zone_store_closed");return m_cpuZones->Append(parent,&row);}
void OfflineConversionStorage::ReadCpuZone(uint64_t id,OfflineCpuZone& row){Check();if(!m_cpuZones)throw std::runtime_error("cpu_zone_store_missing");m_cpuZones->Read(id,&row);}
void OfflineConversionStorage::PutCpuZone(uint64_t id,const OfflineCpuZone& row){Check();if(m_sealed||m_cpuZonesSealed||!m_cpuZones)throw std::runtime_error("cpu_zone_store_closed");m_cpuZones->Put(id,&row);}
void OfflineConversionStorage::SealCpuZones(){Check();if(m_cpuZonesSealed)return;if(m_cpuZones)m_cpuZones->Seal();m_cpuZonesSealed=true;}
void OfflineConversionStorage::VisitCpuZones(uint64_t root,const std::function<void(uint64_t,const OfflineCpuZone&,uint32_t,bool)>& visitor){Check();if(!m_cpuZonesSealed||!m_cpuZones)throw std::runtime_error("cpu_zone_store_not_sealed");m_cpuZones->Visit(root,[&](uint64_t id,const void* bytes,uint32_t children,bool begin){OfflineCpuZone row;std::memcpy(&row,bytes,sizeof(row));visitor(id,row,children,begin);});}
uint64_t OfflineConversionStorage::CreateGpuThread(){Check();if(m_sealed||m_gpuZonesSealed)throw std::runtime_error("gpu_zone_store_closed");if(!m_gpuZones)m_gpuZones=std::make_unique<storage::OrderedForest>(m_root/"gpu-zones.bin",sizeof(OfflineGpuZone),m_resources);return m_gpuZones->CreateRoot();}
uint64_t OfflineConversionStorage::AppendGpuZone(uint64_t parent,const OfflineGpuZone& row){Check();if(m_sealed||m_gpuZonesSealed||!m_gpuZones)throw std::runtime_error("gpu_zone_store_closed");return m_gpuZones->Append(parent,&row);}
void OfflineConversionStorage::ReadGpuZone(uint64_t id,OfflineGpuZone& row){Check();if(!m_gpuZones)throw std::runtime_error("gpu_zone_store_missing");m_gpuZones->Read(id,&row);}
void OfflineConversionStorage::PutGpuZone(uint64_t id,const OfflineGpuZone& row){Check();if(m_sealed||m_gpuZonesSealed||!m_gpuZones)throw std::runtime_error("gpu_zone_store_closed");m_gpuZones->Put(id,&row);}
void OfflineConversionStorage::SealGpuZones(){Check();if(m_gpuZonesSealed)return;if(m_gpuZones)m_gpuZones->Seal();m_gpuZonesSealed=true;}
void OfflineConversionStorage::VisitGpuZones(uint64_t root,const std::function<void(uint64_t,const OfflineGpuZone&,uint32_t,bool)>& visitor){Check();if(!m_gpuZonesSealed||!m_gpuZones)throw std::runtime_error("gpu_zone_store_not_sealed");m_gpuZones->Visit(root,[&](uint64_t id,const void* bytes,uint32_t children,bool begin){OfflineGpuZone row;std::memcpy(&row,bytes,sizeof(row));visitor(id,row,children,begin);});}
}
