#ifndef TRACY_RESOURCE_INTERVALS_HPP
#define TRACY_RESOURCE_INTERVALS_HPP
#include "TracyDiskIndex64.hpp"
#include "TracyMappedAppendArena.hpp"
#include "../public/common/TracyJnGpuCatalog.hpp"
namespace tracy::storage {
class ResourceIntervals {
    struct Interval {uint64_t resource=0;int64_t begin=0,end=0;uint64_t next=0;};
    static_assert(sizeof(Interval)==32);
    static constexpr uint64_t SmallEntries=4096,MappedBase=1ull<<32;
    ConversionResources& m_resources;analysis::AnalysisWorkspaceReservation m_control,m_smallLease;
    std::array<std::filesystem::path,2> m_paths;
    std::unique_ptr<DiskIndex64> m_keys;std::unique_ptr<MappedAppendArena> m_intervals;
    std::vector<Interval> m_small;
    uint64_t m_count=0;
    bool m_sealed=false,m_closed=false;
public:
    struct Stats {uint64_t adds=0,resolves=0,intervalNodeVisits=0,maxNodesPerResolve=0,smallIntervalBytes=0,mappedIntervalBytes=0,mappedIntervalChunks=0;};
private:
    Stats m_stats{};
    static void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
    // The open interval is always the latest interval, so one flag replaces a second state table.
    uint64_t Head(uint64_t encoded){const auto id=encoded&~uint64_t(1);Require(id!=0,"resource_interval_state_index");return id;}
    static std::pair<uint32_t,uint32_t> Decode(uint64_t encoded){Require(encoded>=MappedBase,"resource_interval_index");const auto high=uint32_t(encoded>>32),offset=uint32_t(encoded);Require(high>0&&(offset&7)==0,"resource_interval_index");return{high-1,offset};}
    Interval ReadInterval(uint64_t encoded){Interval value;if(encoded<MappedBase){Require((encoded&7)==0&&encoded>=8&&encoded/8<=m_small.size(),"resource_interval_small_index");value=m_small[size_t(encoded/8-1)];}else{const auto [chunk,offset]=Decode(encoded);Require(m_intervals!=nullptr,"resource_interval_mapping");std::memcpy(&value,m_intervals->GetRaw(chunk,offset,sizeof(value)),sizeof(value));}Require(value.next<encoded&&value.resource!=0,"resource_interval_chain");return value;}
    void WriteInterval(uint64_t encoded,const Interval& value){if(encoded<MappedBase){Require((encoded&7)==0&&encoded>=8&&encoded/8<=m_small.size(),"resource_interval_small_index");m_small[size_t(encoded/8-1)]=value;}else{const auto [chunk,offset]=Decode(encoded);Require(m_intervals!=nullptr,"resource_interval_mapping");std::memcpy(m_intervals->GetRawMutable(chunk,offset,sizeof(value)),&value,sizeof(value));}}
    uint64_t AppendInterval(const Interval& value){Require(m_count<UINT64_MAX,"resource_interval_count");if(m_small.size()<SmallEntries){if(m_small.empty()){m_smallLease.Resize(SmallEntries*sizeof(Interval)+256);m_small.reserve(SmallEntries);}m_small.push_back(value);++m_count;return uint64_t(m_small.size())*8;}if(!m_intervals)m_intervals=std::make_unique<MappedAppendArena>(m_paths[1]/"interval.rows",m_resources);const auto location=m_intervals->Allocate(sizeof(value),alignof(Interval));Require(location.chunk<UINT32_MAX&&location.offset<=UINT32_MAX,"resource_interval_location");const auto encoded=(uint64_t(location.chunk+1)<<32)|uint32_t(location.offset);Require(encoded>=MappedBase&&(encoded&1)==0,"resource_interval_alignment");std::memcpy(location.data,&value,sizeof(value));++m_count;return encoded;}
public:
    ResourceIntervals(std::filesystem::path root,ConversionResources& resources,std::string identity=std::string(64,'0'))
        :m_resources(resources),m_control(resources.Workspace(),16384),m_smallLease(resources.Workspace()){
        m_paths={root/"pointer.index",root};
        Require(!std::filesystem::exists(analysis::AnalysisIoPath(root)),"resource_intervals_exists");resources.BeforeFileGrowth(root,0);std::filesystem::create_directories(analysis::AnalysisIoPath(root));
        m_keys=std::make_unique<DiskIndex64>(m_paths[0],std::move(identity),resources);
    }
    ~ResourceIntervals(){CloseAndRemove();}
    void Add(const JnGpuCatalogResourceRecordV1& record){
        m_resources.CheckFast();Require(!m_closed&&!m_sealed,"resource_intervals_add_state");++m_stats.adds;if(!record.pointerToken||!record.resourceId)return;
        const auto encoded=m_keys->Find(record.pointerToken);const auto head=encoded?Head(*encoded):0;
        const auto operation=JnGpuCatalogRecordOperation(record.operation);
        if(operation==JnGpuCatalogRecordOperation::Destroy){
            if(!encoded||!(*encoded&1))return;auto interval=ReadInterval(head);
            if(interval.resource==record.resourceId){interval.end=record.time;WriteInterval(head,interval);}
            m_keys->Upsert(record.pointerToken,head);return;
        }
        const bool create=operation==JnGpuCatalogRecordOperation::Create;
        if(!create&&operation!=JnGpuCatalogRecordOperation::Open&&operation!=JnGpuCatalogRecordOperation::Snapshot&&encoded)return;
        Interval interval{record.resourceId,create?record.time:INT64_MIN,INT64_MAX,head};
        const auto id=AppendInterval(interval);m_keys->Upsert(record.pointerToken,id|1);
    }
    void Seal(){m_resources.CheckFast();Require(!m_closed&&!m_sealed,"resource_intervals_seal_state");m_keys->Seal();m_sealed=true;}
    uint64_t Resolve(uint64_t pointer,int64_t time){
        m_resources.CheckFast();Require(!m_closed&&m_sealed,"resource_intervals_resolve_state");++m_stats.resolves;if(!pointer)return 0;const auto encoded=m_keys->Find(pointer);if(!encoded)return 0;
        auto next=Head(*encoded);uint64_t result=0,chain=0;
        while(next){auto interval=ReadInterval(next);next=interval.next;++chain;++m_stats.intervalNodeVisits;if((chain&1023)==0)m_resources.CheckFast();
            if(time!=INT64_MIN&&(time<interval.begin||time>interval.end))continue;
            if(result&&result!=interval.resource){m_stats.maxNodesPerResolve=std::max(m_stats.maxNodesPerResolve,chain);return 0;}result=interval.resource;
        }
        m_stats.maxNodesPerResolve=std::max(m_stats.maxNodesPerResolve,chain);
        return result;
    }
    bool CloseAndRemove() noexcept {
        if(m_closed)return true;m_keys.reset();bool ok=true;if(m_intervals&&!m_intervals->CloseAndRemove())ok=false;m_intervals.reset();std::vector<Interval>().swap(m_small);m_smallLease.Resize(0);
        for(size_t i=0;i<m_paths.size();++i)if(!m_resources.RemoveOwned(m_paths[i]))ok=false;
        m_closed=true;if(!ok)m_resources.Fail("resource_interval_cleanup_failed");return ok;
    }
    uint64_t Count()const{return m_count;}
    uint64_t PointerCount()const{return m_keys?m_keys->Count():0;}
    uint64_t LogicalBytes()const{return (m_keys?m_keys->LogicalBytes():0)+(m_intervals?m_intervals->ReservedBytes():0);}
    Stats Statistics()const{auto value=m_stats;value.smallIntervalBytes=m_small.size()*sizeof(Interval);if(m_intervals){value.mappedIntervalBytes=m_intervals->UsedBytes();value.mappedIntervalChunks=m_intervals->ChunkCount();}return value;}
};
}
#endif
