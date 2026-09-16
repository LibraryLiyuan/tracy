#ifndef TRACY_RESOURCE_INTERVALS_HPP
#define TRACY_RESOURCE_INTERVALS_HPP
#include "TracyDiskIndex64.hpp"
#include "TracyMutableRows.hpp"
#include "../public/common/TracyJnGpuCatalog.hpp"
namespace tracy::storage {
class ResourceIntervals {
    struct Interval {uint64_t resource=0;int64_t begin=0,end=0;uint64_t next=0;};
    static_assert(sizeof(Interval)==32);
    ConversionResources& m_resources;analysis::AnalysisWorkspaceReservation m_control;
    std::array<std::filesystem::path,3> m_paths;
    std::unique_ptr<DiskIndex64> m_keys;std::unique_ptr<MutableRows> m_intervals;
    bool m_sealed=false,m_closed=false;
    static void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
    // The open interval is always the latest interval, so one flag replaces a second state table.
    uint64_t Head(uint64_t encoded){auto id=encoded>>1;Require(id>0&&id<=m_intervals->Count(),"resource_interval_state_index");return id;}
    Interval ReadInterval(uint64_t encoded){Require(encoded>0&&encoded<=m_intervals->Count(),"resource_interval_index");Interval value;m_intervals->Read(encoded-1,&value);Require(value.next<encoded&&value.resource!=0,"resource_interval_chain");return value;}
public:
    ResourceIntervals(std::filesystem::path root,ConversionResources& resources,std::string identity=std::string(64,'0'))
        :m_resources(resources),m_control(resources.Workspace(),16384){
        m_paths={root/"pointer.index",root/"interval.rows",root};
        Require(!std::filesystem::exists(analysis::AnalysisIoPath(root)),"resource_intervals_exists");resources.BeforeFileGrowth(root,0);std::filesystem::create_directories(analysis::AnalysisIoPath(root));
        m_keys=std::make_unique<DiskIndex64>(m_paths[0],std::move(identity),resources);
        m_intervals=std::make_unique<MutableRows>(m_paths[1],sizeof(Interval),resources);
    }
    ~ResourceIntervals(){CloseAndRemove();}
    void Add(const JnGpuCatalogResourceRecordV1& record){
        m_resources.CheckFast();Require(!m_closed&&!m_sealed,"resource_intervals_add_state");if(!record.pointerToken||!record.resourceId)return;
        const auto encoded=m_keys->Find(record.pointerToken);const auto head=encoded?Head(*encoded):0;
        const auto operation=JnGpuCatalogRecordOperation(record.operation);
        if(operation==JnGpuCatalogRecordOperation::Destroy){
            if(!encoded||!(*encoded&1))return;auto interval=ReadInterval(head);
            if(interval.resource==record.resourceId){interval.end=record.time;m_intervals->Put(head-1,&interval);}
            m_keys->Upsert(record.pointerToken,head<<1);return;
        }
        const bool create=operation==JnGpuCatalogRecordOperation::Create;
        if(!create&&operation!=JnGpuCatalogRecordOperation::Open&&operation!=JnGpuCatalogRecordOperation::Snapshot&&encoded)return;
        Require(m_intervals->Count()<(UINT64_MAX>>1),"resource_interval_temporary_id_range");
        Interval interval{record.resourceId,create?record.time:INT64_MIN,INT64_MAX,head};
        const auto id=m_intervals->Append(&interval)+1;m_keys->Upsert(record.pointerToken,(id<<1)|1);
    }
    void Seal(){m_resources.CheckFast();Require(!m_closed&&!m_sealed,"resource_intervals_seal_state");m_keys->Seal();m_intervals->Seal();m_sealed=true;}
    uint64_t Resolve(uint64_t pointer,int64_t time){
        m_resources.CheckFast();Require(!m_closed&&m_sealed,"resource_intervals_resolve_state");if(!pointer)return 0;const auto encoded=m_keys->Find(pointer);if(!encoded)return 0;
        auto next=Head(*encoded);uint64_t result=0;
        while(next){m_resources.CheckFast();auto interval=ReadInterval(next);next=interval.next;
            if(time!=INT64_MIN&&(time<interval.begin||time>interval.end))continue;
            if(result&&result!=interval.resource)return 0;result=interval.resource;
        }
        return result;
    }
    bool CloseAndRemove() noexcept {
        if(m_closed)return true;m_keys.reset();m_intervals.reset();bool ok=true;
        for(const auto& path:m_paths)if(!m_resources.RemoveOwned(path))ok=false;
        m_closed=true;if(!ok)m_resources.Fail("resource_interval_cleanup_failed");return ok;
    }
    uint64_t Count()const{return m_intervals?m_intervals->Count():0;}
    uint64_t PointerCount()const{return m_keys?m_keys->Count():0;}
    uint64_t LogicalBytes()const{return (m_keys?m_keys->LogicalBytes():0)+(m_intervals?m_intervals->LogicalBytes():0);}
};
}
#endif
