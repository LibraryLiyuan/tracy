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
#pragma pack(push,1)
struct CpuZoneWriteBeginRow{uint8_t tag=1;uint64_t thread=0;int64_t start=0;int16_t sourceLocation=0;uint32_t extra=0,children=0;};
struct CpuZoneWriteEndRow{uint8_t tag=2;int64_t end=0;};
struct GpuZoneWriteBeginRow{uint8_t tag=3;uint32_t context=0;uint64_t thread=0;int64_t cpuStart=0,gpuStart=0;int16_t sourceLocation=0;uint32_t callstack=0;uint64_t children=0;};
struct GpuZoneWriteEndRow{uint8_t tag=4;int64_t cpuEnd=0,gpuEnd=0;uint16_t queryId=0;};
#pragma pack(pop)
static_assert(sizeof(CpuZoneWriteBeginRow)==27&&sizeof(CpuZoneWriteEndRow)==9&&sizeof(GpuZoneWriteBeginRow)==43&&sizeof(GpuZoneWriteEndRow)==19);
analysis::PagedRecordOptions OfflineConversionStorage::Options()
{
    analysis::PagedRecordOptions o;o.workspace=m_resources.Workspace();o.scratch=m_pagedScratch;
    o.beforeMemoryAllocation=[this](uint64_t bytes){m_resources.Check(bytes);};
    o.cancelled=[this]{m_resources.CheckFast();return false;};
    o.beforeFileGrowth=[this](const auto& p,uint64_t n){m_resources.BeforeFileGrowth(p,n);};
    o.renameFile=[this](const auto& a,const auto& b){m_resources.RenameOwned(a,b);};return o;
}
OfflineConversionStorage::OfflineConversionStorage(std::filesystem::path root,std::string identity,storage::ConversionResources& resources)
    :m_root(std::move(root)),m_identity(std::move(identity)),m_resources(resources),m_pagedScratch(std::make_shared<analysis::PagedRecordScratch>(resources.Workspace())),
     m_expandLease(resources.Workspace(),65536),m_definitionLease(resources.Workspace()),m_sampleHashLease(resources.Workspace(),sizeof(m_sampleHashBuffer)+256),m_sampleHashCacheLease(resources.Workspace(),256*1024),m_zoneWriteHashLease(resources.Workspace(),sizeof(m_zoneWriteHashBuffer)+256),m_members(1024),m_expanded(1024)
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
    if(count>m_definitionMembers.capacity()){
        m_definitionLease.Resize(bytes+256);m_definitionMembers.reserve(count);
        m_definitionLease.Resize(uint64_t(m_definitionMembers.capacity())*sizeof(storage::ResourceSetMember)+256);
    }
    m_definitionMembers.resize(count);std::memcpy(m_definitionMembers.data(),data,bytes);
    if(!m_definitions)m_definitions=std::make_unique<storage::ResourceSetStore>(m_root/"sets",m_identity,m_resources);
    const auto inserted=m_definitions->Define(id,m_definitionMembers);if(inserted)++m_definitionCount;return inserted;
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
    m_jobs.reset();m_uses.reset();if(m_definitions)m_resourceSetStats=m_definitions->Statistics();m_definitions.reset();m_definitionMembers.clear();m_definitionMembers.shrink_to_fit();m_definitionLease.Resize(0);m_members.clear();m_members.shrink_to_fit();m_expanded.clear();m_expanded.shrink_to_fit();m_expandLease.Resize(0);
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
        uint64_t lastProgress=0;const auto update=[&]{if(m_passTimeStats.sourceRows-lastProgress>=65536||m_passTimeStats.sourceRows==total){lastProgress=m_passTimeStats.sourceRows;ReportFinalizeProgress("pass_time_build",m_passTimeStats.sourceRows,total,"source_rows");}};
        VisitRecords(OfflineAppendDomain::GpuReferencePass,[&](const void* p,uint64_t n){m_passTimeStats.sourceRows+=n;auto rows=static_cast<const JnGpuReferencePassData*>(p);for(uint64_t i=0;i<n;++i)if(rows[i].passId){++m_passTimeStats.candidateRows;++m_passTimeStats.upserts;index->Upsert(rows[i].passId,std::bit_cast<uint64_t>(rows[i].time));}update();});
        VisitRecords(OfflineAppendDomain::GfxEntity,[&](const void* p,uint64_t n){m_passTimeStats.sourceRows+=n;auto rows=static_cast<const JnGfxEntityData*>(p);for(uint64_t i=0;i<n;++i)if(rows[i].entityId&&rows[i].kind==uint8_t(JnGfxEntityKind::ExplicitGpuPass)){++m_passTimeStats.candidateRows;++m_passTimeStats.upserts;index->Upsert(rows[i].entityId,std::bit_cast<uint64_t>(rows[i].time));}update();});
        VisitRecords(OfflineAppendDomain::GfxLink,[&](const void* p,uint64_t n){m_passTimeStats.sourceRows+=n;auto rows=static_cast<const JnGfxLinkData*>(p);for(uint64_t i=0;i<n;++i)if(rows[i].sourceId&&rows[i].relation==uint8_t(JnGfxRelation::RangeEvidenceComplete)){++m_passTimeStats.candidateRows;++m_passTimeStats.upserts;index->Upsert(rows[i].sourceId,std::bit_cast<uint64_t>(rows[i].time));}update();});
        m_passTimeStats.uniqueKeys=index->Count();m_passTimeStats.buildNanoseconds=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count());
        const auto sealStarted=std::chrono::steady_clock::now();const auto seal=index->Seal([&](storage::MappedHashIndex64::SealPhase phase,uint64_t completed,uint64_t amount){if(phase==storage::MappedHashIndex64::SealPhase::Digest)ReportFinalizeProgress("pass_time_digest",completed,amount,"bytes");else ReportFinalizeProgress("pass_time_flush",completed,amount,"steps");});m_passTimeStats.sealNanoseconds=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-sealStarted).count());m_passTimeStats.digestNanoseconds=seal.digestNanoseconds;m_passTimeStats.flushNanoseconds=seal.flushNanoseconds;m_passTimeStats.digestBytes=seal.digestBytes;
        m_passTimes=std::move(index);m_passTimesPrepared=true;m_passTimeStats.buildCount=1;return m_passTimeStats;
    }catch(...){if(!m_resources.Cancelled())m_resources.Fail("pass_index_build_failed");throw;}
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
void OfflineConversionStorage::EndResourceIntervals() noexcept {if(m_resourceIntervals){m_intervalPeaks[0]=std::max(m_intervalPeaks[0],m_resourceIntervals->Count());m_intervalPeaks[1]=std::max(m_intervalPeaks[1],m_resourceIntervals->PointerCount());m_intervalPeaks[2]=std::max(m_intervalPeaks[2],m_resourceIntervals->LogicalBytes());const auto stats=m_resourceIntervals->Statistics();m_intervalStats.adds+=stats.adds;m_intervalStats.resolves+=stats.resolves;m_intervalStats.intervalNodeVisits+=stats.intervalNodeVisits;m_intervalStats.maxNodesPerResolve=std::max(m_intervalStats.maxNodesPerResolve,stats.maxNodesPerResolve);m_intervalStats.smallIntervalBytes=std::max(m_intervalStats.smallIntervalBytes,stats.smallIntervalBytes);m_intervalStats.mappedIntervalBytes=std::max(m_intervalStats.mappedIntervalBytes,stats.mappedIntervalBytes);m_intervalStats.mappedIntervalChunks=std::max(m_intervalStats.mappedIntervalChunks,stats.mappedIntervalChunks);m_resourceIntervals.reset();}}
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
const uint64_t* OfflineConversionStorage::CallstackFrames(uint32_t id,uint32_t& count){Check();if(!m_callstacks||!id||id>=m_callstacks->Count()){count=0;return nullptr;}const auto value=m_callstacks->Get(id);count=value.count;return value.frames;}
std::string OfflineConversionStorage::CallstackHash()const{if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");return m_callstacks?m_callstacks->Hash():analysis::Sha256Builder().FinalHex();}
uint64_t OfflineConversionStorage::AppendSample(uint64_t thread,uint8_t kind,int64_t time,uint32_t callstack){Check();if(m_sealed)throw std::runtime_error("offline_store_closed");if(!m_samples)m_samples=std::make_unique<storage::OfflineSampleStore>(m_root/"samples",m_identity,m_resources);return m_samples->Append(thread,kind,{time,callstack});}
bool OfflineConversionStorage::FindSample(uint64_t thread,uint8_t kind,int64_t time,uint64_t& id,uint32_t& callstack){Check();if(m_sealed||!m_samples)return false;const auto value=m_samples->Find(thread,kind,time);if(!value)return false;id=value->first;callstack=value->second.callstack;return true;}
void OfflineConversionStorage::PutSample(uint64_t id,int64_t time,uint32_t callstack){Check();if(m_sealed||!m_samples)throw std::runtime_error("offline_sample_update_state");m_samples->Put(id,{time,callstack});}
std::optional<int64_t> OfflineConversionStorage::LastSampleTime(uint64_t thread,uint8_t kind)const{return m_samples?m_samples->LastTime(thread,kind):std::nullopt;}
uint64_t OfflineConversionStorage::SampleCount(uint64_t thread,uint8_t kind)const{return m_samples?m_samples->Count(thread,kind):0;}
void OfflineConversionStorage::VisitSamples(uint64_t thread,uint8_t kind,const std::function<void(int64_t,uint32_t)>& visitor){
    Check();if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");analysis::Sha256Builder hash;size_t buffered=0;uint64_t seen=0;
    if(m_samples)m_samples->Visit(thread,kind,[&](const storage::OfflineSampleRow& row){m_sampleHashBuffer[buffered++]=row;if(buffered==m_sampleHashBuffer.size()){hash.Update(m_sampleHashBuffer.data(),buffered*sizeof(storage::OfflineSampleRow));buffered=0;}visitor(row.time,row.callstack);++seen;});
    if(buffered)hash.Update(m_sampleHashBuffer.data(),buffered*sizeof(storage::OfflineSampleRow));const auto expected=SampleCount(thread,kind);if(seen!=expected)throw std::runtime_error("offline_sample_visit_count:"+std::to_string(thread)+":"+std::to_string(kind)+":"+std::to_string(seen)+":"+std::to_string(expected));
    const auto key=std::make_pair(thread,kind);if(!m_sampleHashes.contains(key)){if((m_sampleHashes.size()+1)*256>m_sampleHashCacheLease.Bytes())m_sampleHashCacheLease.Resize((m_sampleHashes.size()+1)*256);m_sampleHashes.emplace(key,hash.FinalHex());}
}
std::string OfflineConversionStorage::SampleHash(uint64_t thread,uint8_t kind){
    const auto key=std::make_pair(thread,kind);const auto found=m_sampleHashes.find(key);if(found!=m_sampleHashes.end())return found->second;
    analysis::Sha256Builder hash;size_t buffered=0;uint64_t seen=0;if(m_samples)m_samples->Visit(thread,kind,[&](const storage::OfflineSampleRow& row){m_sampleHashBuffer[buffered++]=row;if(buffered==m_sampleHashBuffer.size()){hash.Update(m_sampleHashBuffer.data(),buffered*sizeof(storage::OfflineSampleRow));buffered=0;}++seen;});if(buffered)hash.Update(m_sampleHashBuffer.data(),buffered*sizeof(storage::OfflineSampleRow));if(seen!=SampleCount(thread,kind))throw std::runtime_error("offline_sample_hash_count");const auto value=hash.FinalHex();if((m_sampleHashes.size()+1)*256>m_sampleHashCacheLease.Bytes())m_sampleHashCacheLease.Resize((m_sampleHashes.size()+1)*256);m_sampleHashes.emplace(key,value);return value;
}
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
void OfflineConversionStorage::AppendZoneWriteHash(const void* data,size_t bytes){
    if(!m_zoneWriteHash||!data||bytes>m_zoneWriteHashBuffer.size())throw std::runtime_error("offline_zone_write_hash_state");const auto* src=static_cast<const unsigned char*>(data);
    while(bytes){const auto take=std::min(bytes,m_zoneWriteHashBuffer.size()-m_zoneWriteHashBytes);std::memcpy(m_zoneWriteHashBuffer.data()+m_zoneWriteHashBytes,src,take);m_zoneWriteHashBytes+=take;src+=take;bytes-=take;if(m_zoneWriteHashBytes==m_zoneWriteHashBuffer.size()){m_zoneWriteHash->Update(m_zoneWriteHashBuffer.data(),m_zoneWriteHashBytes);m_zoneWriteHashBytes=0;}}
}
std::string OfflineConversionStorage::FinishZoneWriteHash(uint8_t kind){
    if(m_zoneWriteKind!=kind||!m_zoneWriteHash||m_zoneWriteSeen!=m_zoneWriteExpected)throw std::runtime_error("offline_zone_write_hash_count");if(m_zoneWriteHashBytes)m_zoneWriteHash->Update(m_zoneWriteHashBuffer.data(),m_zoneWriteHashBytes);auto value=m_zoneWriteHash->FinalHex();m_zoneWriteHash.reset();m_zoneWriteHashBytes=0;m_zoneWriteKind=0;return value;
}
void OfflineConversionStorage::BeginCpuZoneWrite(uint64_t count){Check();if(!m_sealed||m_zoneWriteKind)throw std::runtime_error("offline_cpu_zone_write_begin");m_zoneWriteKind=1;m_zoneWriteExpected=count;m_zoneWriteSeen=0;m_zoneWriteHashBytes=0;m_zoneWriteHash=std::make_unique<analysis::Sha256Builder>();}
void OfflineConversionStorage::RecordCpuZoneWriteBegin(uint64_t thread,int64_t start,int16_t source,uint32_t extra,uint32_t children){if(m_zoneWriteKind!=1||m_zoneWriteSeen>=m_zoneWriteExpected)throw std::runtime_error("offline_cpu_zone_write_record");const CpuZoneWriteBeginRow row{1,thread,start,source,extra,children};AppendZoneWriteHash(&row,sizeof(row));++m_zoneWriteSeen;}
void OfflineConversionStorage::RecordCpuZoneWriteEnd(int64_t end){if(m_zoneWriteKind!=1)throw std::runtime_error("offline_cpu_zone_write_record");const CpuZoneWriteEndRow row{2,end};AppendZoneWriteHash(&row,sizeof(row));}
void OfflineConversionStorage::EndCpuZoneWrite(){m_cpuZoneWriteCount=m_zoneWriteExpected;m_cpuZoneWriteHash=FinishZoneWriteHash(1);}
std::string OfflineConversionStorage::CpuZoneWriteHash()const{if(m_cpuZoneWriteHash.size()==64)return m_cpuZoneWriteHash;if(CpuZoneCount()==0)return analysis::Sha256Builder().FinalHex();throw std::runtime_error("offline_cpu_zone_write_hash");}
void OfflineConversionStorage::BeginGpuZoneWrite(uint64_t count){Check();if(!m_sealed||m_zoneWriteKind)throw std::runtime_error("offline_gpu_zone_write_begin");m_zoneWriteKind=2;m_zoneWriteExpected=count;m_zoneWriteSeen=0;m_zoneWriteHashBytes=0;m_zoneWriteHash=std::make_unique<analysis::Sha256Builder>();}
void OfflineConversionStorage::RecordGpuZoneWriteBegin(uint32_t context,uint64_t thread,int64_t cpuStart,int64_t gpuStart,int16_t source,uint32_t callstack,uint64_t children){if(m_zoneWriteKind!=2||m_zoneWriteSeen>=m_zoneWriteExpected)throw std::runtime_error("offline_gpu_zone_write_record");const GpuZoneWriteBeginRow row{3,context,thread,cpuStart,gpuStart,source,callstack,children};AppendZoneWriteHash(&row,sizeof(row));++m_zoneWriteSeen;}
void OfflineConversionStorage::RecordGpuZoneWriteEnd(int64_t cpuEnd,int64_t gpuEnd,uint16_t query){if(m_zoneWriteKind!=2)throw std::runtime_error("offline_gpu_zone_write_record");const GpuZoneWriteEndRow row{4,cpuEnd,gpuEnd,query};AppendZoneWriteHash(&row,sizeof(row));}
void OfflineConversionStorage::EndGpuZoneWrite(){m_gpuZoneWriteCount=m_zoneWriteExpected;m_gpuZoneWriteHash=FinishZoneWriteHash(2);}
std::string OfflineConversionStorage::GpuZoneWriteHash()const{if(m_gpuZoneWriteHash.size()==64)return m_gpuZoneWriteHash;if(GpuZoneCount()==0)return analysis::Sha256Builder().FinalHex();throw std::runtime_error("offline_gpu_zone_write_hash");}
}
