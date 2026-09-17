#ifndef TRACY_OFFLINE_SAMPLE_STORE_HPP
#define TRACY_OFFLINE_SAMPLE_STORE_HPP

#include "TracyDiskIndex64.hpp"
#include "TracyMutableRows.hpp"
#include "../analysis/TracyAnalysisIoPath.hpp"
#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace tracy::storage
{

#pragma pack(push,1)
struct OfflineSampleRow
{
    int64_t time = 0;
    uint32_t callstack = 0;
};
struct OfflineSampleLink
{
    OfflineSampleRow value;
    uint64_t next = 0;
};
#pragma pack(pop)
static_assert( sizeof( OfflineSampleRow ) == 12 && sizeof( OfflineSampleLink ) == 20 );

struct OfflineSampleStoreStats
{
    uint64_t appends = 0, finds = 0, updates = 0, visits = 0, visitedRows = 0;
    uint64_t indexBuilds = 0, indexBuildRows = 0, indexBuildNanoseconds = 0, findNanoseconds = 0;
};

class OfflineSampleStore
{
    struct Key
    {
        uint64_t thread;
        uint8_t kind;
        bool operator==( const Key& other ) const { return thread == other.thread && kind == other.kind; }
    };
    struct KeyHash { size_t operator()( const Key& key ) const { return size_t( key.thread ^ ( uint64_t( key.kind ) * 0x9E3779B97F4A7C15ull ) ); } };
    struct State
    {
        uint64_t first = 0, last = 0, zeroFirst = 0, zeroLast = 0, count = 0;
        int64_t lastTime = -1;
        uint32_t ordinal = 0;
        bool indexed = false;
    };
public:
    OfflineSampleStore( std::filesystem::path root, std::string identity, ConversionResources& resources )
        : m_root( std::move( root ) ), m_identity( std::move( identity ) ), m_resources( resources ), m_stateLease( resources.Workspace() )
    {
        Require( m_identity.size() == 64, "offline_sample_identity" );
        Require( !std::filesystem::exists( analysis::AnalysisIoPath( m_root ) ), "offline_sample_exists" );
        m_resources.BeforeFileGrowth( m_root, 0 );std::filesystem::create_directories( analysis::AnalysisIoPath( m_root ) );
        m_rows = std::make_unique<MutableRows>( m_root / "rows.bin", sizeof( OfflineSampleLink ), m_resources );
    }
    uint64_t Append( uint64_t thread, uint8_t kind, const OfflineSampleRow& value )
    {
        Check();Require( !m_sealed && kind < 2 && value.time >= 0 && value.time < ( int64_t( 1 ) << 47 ), "offline_sample_append" );++m_stats.appends;
        auto& state = GetState( thread, kind );OfflineSampleLink row;row.value=value;const auto id=m_rows->Append(&row);const bool appendOrdered=kind==1||value.time>0;auto& first=appendOrdered?state.first:state.zeroFirst;auto& last=appendOrdered?state.last:state.zeroLast;
        if(last){OfflineSampleLink prior;m_rows->Read(last-1,&prior);Require(prior.next==0,"offline_sample_tail");prior.next=id+1;m_rows->Put(last-1,&prior);}else first=id+1;last=id+1;
        if(value.time>0&&state.indexed){const auto indexed=m_timeIndex->Count();m_timeIndex->Upsert(IndexKey(state,value.time),id);if(kind==0&&m_timeIndex->Count()!=indexed+1)throw std::runtime_error("offline_sample_duplicate_index:"+std::to_string(thread)+":"+std::to_string(kind)+":"+std::to_string(value.time));}
        state.lastTime=std::max(state.lastTime,value.time);++state.count;++m_total;return id;
    }
    std::optional<std::pair<uint64_t,OfflineSampleRow>> Find( uint64_t thread, uint8_t kind, int64_t time )
    {
        const auto started=std::chrono::steady_clock::now();Check();Require(!m_sealed&&kind<2&&time>0&&time<(int64_t(1)<<47),"offline_sample_find");++m_stats.finds;auto found=m_states.find({thread,kind});if(found==m_states.end()){RecordFindTime(started);return{};}EnsureIndex(found->second);const auto id=m_timeIndex->Find(IndexKey(found->second,time));if(!id){RecordFindTime(started);return{};}OfflineSampleLink row;m_rows->Read(*id,&row);Require(row.value.time==time,"offline_sample_index_value");RecordFindTime(started);return std::make_pair(*id,row.value);
    }
    void Put( uint64_t id, const OfflineSampleRow& value ){Check();Require(!m_sealed&&id<m_rows->Count(),"offline_sample_update");++m_stats.updates;OfflineSampleLink row;m_rows->Read(id,&row);Require(row.value.time==value.time,"offline_sample_update_time");row.value=value;m_rows->Put(id,&row);}
    std::optional<int64_t> LastTime(uint64_t thread,uint8_t kind)const{const auto found=m_states.find({thread,kind});return found==m_states.end()?std::nullopt:std::optional<int64_t>(found->second.lastTime);}
    void Seal(){Check();Require(!m_sealed,"offline_sample_sealed");m_rows->Seal();if(m_timeIndex)m_timeIndex->Seal();m_sealed=true;}
    template<class Visitor>void Visit(uint64_t thread,uint8_t kind,Visitor&& visitor){Check();Require(m_sealed&&kind<2,"offline_sample_not_sealed");const auto found=m_states.find({thread,kind});if(found==m_states.end())return;++m_stats.visits;auto counted=[&](const OfflineSampleRow& row){++m_stats.visitedRows;visitor(row);};const auto& state=found->second;if(kind==1){VisitList(state.first,counted);return;}VisitList(state.zeroFirst,counted);if(state.indexed){const auto first=uint64_t(state.ordinal)<<47;m_timeIndex->VisitRange(first+1,first+((uint64_t(1)<<47)-1),[&](uint64_t,uint64_t id){OfflineSampleLink row;m_rows->Read(id,&row);counted(row.value);});}else VisitList(state.first,counted);}
    uint64_t Count(uint64_t thread,uint8_t kind)const{const auto found=m_states.find({thread,kind});return found==m_states.end()?0:found->second.count;}
    uint64_t TotalCount()const{return m_total;}
    uint64_t ThreadCount()const{return m_threads.size();}
    bool HasTimeIndex()const{return m_timeIndex!=nullptr;}
    OfflineSampleStoreStats Statistics()const{return m_stats;}
    void ReduceCache(){Check();m_rows->ReduceCache();if(m_timeIndex)m_timeIndex->ReduceCache();}
private:
    static void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}void Check()const{m_resources.CheckFast();Require(!m_failed,"offline_sample_failed");}
    State& GetState(uint64_t thread,uint8_t kind){const Key key{thread,kind};auto found=m_states.find(key);if(found!=m_states.end())return found->second;Require(m_states.size()<(1u<<17),"offline_sample_state_count");const bool newThread=m_threads.emplace(thread).second;m_stateLease.Resize((m_states.size()+1)*256+m_threads.size()*128+256);State state;state.ordinal=uint32_t(m_states.size());return m_states.emplace(key,state).first->second;}
    uint64_t IndexKey(const State& state,int64_t time)const{return(uint64_t(state.ordinal)<<47)|uint64_t(time);}
    void EnsureIndex(State& state){if(state.indexed)return;const auto started=std::chrono::steady_clock::now();if(!m_timeIndex)m_timeIndex=std::make_unique<DiskIndex64>(m_root/"time.index",m_identity,m_resources);uint64_t link=state.first,seen=0;while(link){OfflineSampleLink row;m_rows->Read(link-1,&row);Require(row.value.time>0&&++seen<=state.count,"offline_sample_chain");if(m_timeIndex->Find(IndexKey(state,row.value.time)))throw std::runtime_error("offline_sample_duplicate_before_index");m_timeIndex->Upsert(IndexKey(state,row.value.time),link-1);link=row.next;}state.indexed=true;++m_stats.indexBuilds;m_stats.indexBuildRows+=seen;m_stats.indexBuildNanoseconds+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count());}
    void RecordFindTime(std::chrono::steady_clock::time_point started){m_stats.findNanoseconds+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count());}
    template<class Visitor>void VisitList(uint64_t link,Visitor& visitor){uint64_t seen=0;while(link){OfflineSampleLink row;m_rows->Read(link-1,&row);Require(++seen<=m_rows->Count(),"offline_sample_chain");visitor(row.value);link=row.next;}}
    std::filesystem::path m_root;std::string m_identity;ConversionResources& m_resources;analysis::AnalysisWorkspaceReservation m_stateLease;std::unique_ptr<MutableRows>m_rows;std::unique_ptr<DiskIndex64>m_timeIndex;std::unordered_map<Key,State,KeyHash>m_states;std::unordered_set<uint64_t>m_threads;OfflineSampleStoreStats m_stats;uint64_t m_total=0;bool m_sealed=false,m_failed=false;
};

}
#endif
