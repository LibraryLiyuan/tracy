#include "OfflineConversionStorage.hpp"
#include "../../analysis/TracyHash.hpp"
#include "../../analysis/TracyAnalysisIoPath.hpp"
#include "../../storage/TracyZoneExtraCodec.hpp"
#include "../../public/common/TracyQueue.hpp"
#include <algorithm>
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
    if(m_resources.SoftTargetExceeded() && !m_reclaimed) {if(m_definitions)m_definitions->ReduceCaches();if(m_zoneExtras)m_zoneExtras->ReduceCache();if(m_cpuZones)m_cpuZones->ReduceCache();if(m_passTimes)m_passTimes->ReduceCache();if(m_descriptorHeaps)m_descriptorHeaps->ReduceCache();for(auto& table:m_catalogTables)if(table)table->ReduceCache();m_reclaimed=true;}
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
std::optional<int64_t> OfflineConversionStorage::LookupPassTime(uint64_t id)
{
    Check();if(!m_sealed)throw std::runtime_error("pass_index_before_seal");if(!id)return {};
    if(!m_passTimes){
        try {
            auto index=std::make_unique<storage::DiskIndex64>(m_root/"pass-times.index",m_identity,m_resources);
            VisitRecords(OfflineAppendDomain::GpuReferencePass,[&](const void* p,uint64_t n){auto rows=static_cast<const JnGpuReferencePassData*>(p);for(uint64_t i=0;i<n;++i)if(rows[i].passId)index->Upsert(rows[i].passId,std::bit_cast<uint64_t>(rows[i].time));});
            VisitRecords(OfflineAppendDomain::GfxEntity,[&](const void* p,uint64_t n){auto rows=static_cast<const JnGfxEntityData*>(p);for(uint64_t i=0;i<n;++i)if(rows[i].entityId&&rows[i].kind==uint8_t(JnGfxEntityKind::ExplicitGpuPass))index->Upsert(rows[i].entityId,std::bit_cast<uint64_t>(rows[i].time));});
            VisitRecords(OfflineAppendDomain::GfxLink,[&](const void* p,uint64_t n){auto rows=static_cast<const JnGfxLinkData*>(p);for(uint64_t i=0;i<n;++i)if(rows[i].sourceId&&rows[i].relation==uint8_t(JnGfxRelation::RangeEvidenceComplete))index->Upsert(rows[i].sourceId,std::bit_cast<uint64_t>(rows[i].time));});
            index->Seal();m_passTimes=std::move(index);
        }catch(...){m_resources.Fail("pass_index_build_failed");throw;}
    }
    const auto value=m_passTimes->Find(id);return value?std::optional<int64_t>(std::bit_cast<int64_t>(*value)):std::nullopt;
}
void OfflineConversionStorage::BeginResourceIntervals(){Check();if(!m_sealed||m_resourceIntervals)throw std::runtime_error("resource_interval_begin_state");m_resourceIntervals=std::make_unique<storage::ResourceIntervals>(m_root/"resource-intervals",m_resources,m_identity);}
void OfflineConversionStorage::AddResourceInterval(const JnGpuCatalogResourceRecordV1& row){Check();if(!m_resourceIntervals)throw std::runtime_error("resource_interval_missing");m_resourceIntervals->Add(row);}
void OfflineConversionStorage::SealResourceIntervals(){Check();if(!m_resourceIntervals)throw std::runtime_error("resource_interval_missing");m_resourceIntervals->Seal();}
uint64_t OfflineConversionStorage::ResolveResource(uint64_t pointer,int64_t time){Check();if(!m_resourceIntervals)throw std::runtime_error("resource_interval_missing");return m_resourceIntervals->Resolve(pointer,time);}
void OfflineConversionStorage::EndResourceIntervals() noexcept {if(m_resourceIntervals){m_intervalPeaks[0]=std::max(m_intervalPeaks[0],m_resourceIntervals->Count());m_intervalPeaks[1]=std::max(m_intervalPeaks[1],m_resourceIntervals->PointerCount());m_intervalPeaks[2]=std::max(m_intervalPeaks[2],m_resourceIntervals->LogicalBytes());m_resourceIntervals.reset();}}
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
    for(size_t id=0;id<OfflineCatalogDomainCount;++id){analysis::Sha256Builder hash;auto& table=m_catalogTables[id];
        if(table){table->Seal();table->Visit([&](const void* bytes,uint64_t count){
            if(id!=size_t(OfflineCatalogDomain::CatalogString)){hash.Update(bytes,size_t(count)*OfflineCatalogDescriptors[id].width);return;}
            for(uint64_t i=0;i<count;++i){OfflineCatalogString row;std::memcpy(&row,static_cast<const unsigned char*>(bytes)+i*sizeof(row),sizeof(row));if(row.header.byteLength>256)throw std::runtime_error("offline_catalog_string_length");hash.Update(&row.generation,sizeof(row.generation));hash.Update(&row.header,sizeof(row.header));hash.Update(row.bytes.data(),row.header.byteLength);}
        });}
        m_catalogHashes[id]=hash.FinalHex();
    }
    m_catalogQuality=quality;m_catalogSealed=true;
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
    auto options=Options();options.blockBytes=1024*1024;analysis::PagedRecordReader reader(m_root/info.name,m_identity,info.width,options);reader.ValidateAll();
    if(reader.Count()!=table.count)throw std::runtime_error("offline_append_count");
    const auto capacity=std::min<uint64_t>(65536,table.count);const auto bytes=capacity*info.width;
    m_resources.Check(bytes+256);analysis::AnalysisWorkspaceReservation lease(m_resources.Workspace(),bytes+256);std::vector<char> rows(size_t(bytes),0);
    for(uint64_t offset=0;offset<table.count;){Check();auto take=std::min(capacity,table.count-offset);reader.Read(offset,rows.data(),take);visitor(rows.data(),take);offset+=take;}
}
void OfflineConversionStorage::VisitRelations(const std::function<void(const JnRelationData*,uint64_t)>& visitor)
{Check();if(!m_sealed||m_relationFailed)throw std::runtime_error("relation_store_not_sealed");if(m_relationCount)Visit("relations",visitor);}
std::string OfflineConversionStorage::RelationHash() const
{if(!m_sealed||m_relationFailed)throw std::runtime_error("relation_store_not_sealed");return m_relationHash;}
template<class T> void OfflineConversionStorage::Visit(const char* domain,const std::function<void(const T*,uint64_t)>& visitor)
{
    Check();if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");
    analysis::PagedRecordReader reader(m_root/domain,m_identity,uint32_t(sizeof(T)),Options());reader.ValidateAll();
    const auto capacity=std::min<uint64_t>(65536,reader.Count());
    m_resources.Check(capacity*sizeof(T)+256);
    analysis::AnalysisWorkspaceReservation memory(m_resources.Workspace(),capacity*sizeof(T)+256);std::vector<T> rows(size_t(capacity),T{});
    for(uint64_t offset=0;offset<reader.Count();) {
        Check();const auto take=std::min<uint64_t>(capacity,reader.Count()-offset);reader.Read(offset,rows.data(),take);visitor(rows.data(),take);offset+=take;
    }
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
uint64_t OfflineConversionStorage::CreateCpuThread(){Check();if(m_sealed||m_cpuZonesSealed)throw std::runtime_error("cpu_zone_store_closed");if(!m_cpuZones)m_cpuZones=std::make_unique<storage::OrderedForest>(m_root/"cpu-zones.bin",sizeof(OfflineCpuZone),m_resources);return m_cpuZones->CreateRoot();}
uint64_t OfflineConversionStorage::AppendCpuZone(uint64_t parent,const OfflineCpuZone& row){Check();if(m_sealed||m_cpuZonesSealed||!m_cpuZones)throw std::runtime_error("cpu_zone_store_closed");return m_cpuZones->Append(parent,&row);}
void OfflineConversionStorage::ReadCpuZone(uint64_t id,OfflineCpuZone& row){Check();if(!m_cpuZones)throw std::runtime_error("cpu_zone_store_missing");m_cpuZones->Read(id,&row);}
void OfflineConversionStorage::PutCpuZone(uint64_t id,const OfflineCpuZone& row){Check();if(m_sealed||m_cpuZonesSealed||!m_cpuZones)throw std::runtime_error("cpu_zone_store_closed");m_cpuZones->Put(id,&row);}
void OfflineConversionStorage::SealCpuZones(){Check();if(m_cpuZonesSealed)return;if(m_cpuZones)m_cpuZones->Seal();m_cpuZonesSealed=true;}
void OfflineConversionStorage::VisitCpuZones(uint64_t root,const std::function<void(uint64_t,const OfflineCpuZone&,uint32_t,bool)>& visitor){Check();if(!m_cpuZonesSealed||!m_cpuZones)throw std::runtime_error("cpu_zone_store_not_sealed");m_cpuZones->Visit(root,[&](uint64_t id,const void* bytes,uint32_t children,bool begin){OfflineCpuZone row;std::memcpy(&row,bytes,sizeof(row));visitor(id,row,children,begin);});}
}
