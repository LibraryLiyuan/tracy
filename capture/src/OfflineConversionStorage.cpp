#include "OfflineConversionStorage.hpp"
#include "../../analysis/TracyAnalysisIoPath.hpp"
#include <algorithm>
#include <cstring>
namespace tracy::conversion
{
analysis::PagedRecordOptions OfflineConversionStorage::Options()
{
    analysis::PagedRecordOptions o;o.workspace=m_resources.Workspace();
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
    m_jobs=std::make_unique<analysis::PagedRecordWriter>(m_root/"jobs",m_identity,uint32_t(sizeof(JnJobStageData)),Options());
    m_uses=std::make_unique<analysis::PagedRecordWriter>(m_root/"uses",m_identity,uint32_t(sizeof(JnGpuReferenceUseData)),Options());
    m_definitions=std::make_unique<storage::ResourceSetStore>(m_root/"sets",m_identity,resources);
}
void OfflineConversionStorage::Check()
{
    m_resources.CheckFast();
    if(m_resources.SoftTargetExceeded() && !m_reclaimed && m_definitions) {m_definitions->ClearCaches();m_reclaimed=true;}
    if(!m_resources.SoftTargetExceeded())m_reclaimed=false;
}
bool OfflineConversionStorage::DefineResourceSet(uint32_t id,const char* data,size_t bytes)
{
    Check();if(m_sealed)throw std::runtime_error("offline_store_closed");
    if(!data||!bytes||bytes%sizeof(storage::ResourceSetMember)||bytes/sizeof(storage::ResourceSetMember)>65535)return false;
    const auto count=bytes/sizeof(storage::ResourceSetMember);
    analysis::AnalysisWorkspaceReservation memory(m_resources.Workspace(),bytes+256);
    std::vector<storage::ResourceSetMember> members(count);std::memcpy(members.data(),data,bytes);
    const auto inserted=m_definitions->Define(id,members);if(inserted)++m_definitionCount;return inserted;
}
bool OfflineConversionStorage::AppendReferenceSet(uint32_t id,uint16_t count,int64_t time,uint64_t pass,uint64_t thread,uint8_t flags,uint8_t encoding)
{
    Check();if(m_sealed)throw std::runtime_error("offline_store_closed");
    const auto definition=m_definitions->Lookup(id);if(encoding!=2||!definition||definition->count!=count)return false;
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
{Check();if(m_sealed)throw std::runtime_error("offline_store_closed");m_uses->Append(&row,1);++m_useCount;}
void OfflineConversionStorage::AppendJobStage(const JnJobStageData& row)
{Check();if(m_sealed)throw std::runtime_error("offline_store_closed");m_jobs->Append(&row,1);++m_jobCount;}
void OfflineConversionStorage::Seal()
{
    Check();if(m_sealed)throw std::runtime_error("offline_store_closed");
    m_jobs->Commit();m_jobHash=m_jobs->ContentHash();m_uses->Commit();m_useHash=m_uses->ContentHash();m_definitions->Seal();
    m_jobs.reset();m_uses.reset();m_definitions.reset();m_members.clear();m_members.shrink_to_fit();m_expanded.clear();m_expanded.shrink_to_fit();m_expandLease.Resize(0);
    m_sealed=true;
}
template<class T> void OfflineConversionStorage::Visit(const char* domain,const std::function<void(const T*,uint64_t)>& visitor)
{
    Check();if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");
    analysis::PagedRecordReader reader(m_root/domain,m_identity,uint32_t(sizeof(T)),Options());reader.ValidateAll();
    const auto capacity=std::min<uint64_t>(65536,reader.Count());
    analysis::AnalysisWorkspaceReservation memory(m_resources.Workspace(),capacity*sizeof(T)+256);std::vector<T> rows(size_t(capacity),T{});
    for(uint64_t offset=0;offset<reader.Count();) {
        Check();const auto take=std::min<uint64_t>(capacity,reader.Count()-offset);reader.Read(offset,rows.data(),take);visitor(rows.data(),take);offset+=take;
    }
}
void OfflineConversionStorage::VisitReferenceUses(const std::function<void(const JnGpuReferenceUseData*,uint64_t)>& f){Visit("uses",f);}
void OfflineConversionStorage::VisitJobStages(const std::function<void(const JnJobStageData*,uint64_t)>& f){Visit("jobs",f);}
std::string OfflineConversionStorage::JobStageHash() const {if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");return m_jobHash;}
std::string OfflineConversionStorage::ReferenceUseHash() const {if(!m_sealed)throw std::runtime_error("offline_store_not_sealed");return m_useHash;}
}
