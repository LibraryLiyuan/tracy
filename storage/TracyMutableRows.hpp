#ifndef TRACY_MUTABLE_ROWS_HPP
#define TRACY_MUTABLE_ROWS_HPP
#include "TracyConversionResources.hpp"
#include "../analysis/TracyAnalysisIoPath.hpp"
#include "../analysis/TracyHash.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <list>
#include <map>
namespace tracy::storage {
// Fixed-width mutable temporary rows. Callers retain logical IDs, never cache pointers.
class MutableRows {
    static constexpr uint64_t PageBytes=16384,HeaderBytes=128;
    struct Header {uint64_t magic=0x3153574f524e4aull;uint32_t schema=1,width=0;uint64_t id=0,rows=0;char checksum[64]{};char reserved[32]{};};
    static_assert(sizeof(Header)==HeaderBytes);
    struct Slot {
        analysis::AnalysisWorkspaceReservation lease;std::array<unsigned char,PageBytes> bytes{};std::list<uint64_t>::iterator lru;bool dirty=false;
        explicit Slot(ConversionResources& r):lease(r.Workspace(),PageCharge){}
    };
    static constexpr uint64_t PageCharge=sizeof(Slot)+256;
    std::filesystem::path m_path;ConversionResources& m_resources;analysis::AnalysisWorkspaceReservation m_fixedWork;std::fstream m_file;
public:
    struct Stats {uint64_t pageLoads=0,pageWrites=0,evictions=0,evictionCandidates=0,slotAllocations=0,slotReuses=0;};
private:
    std::map<uint64_t,std::unique_ptr<Slot>> m_cache;std::list<uint64_t> m_lru;
    uint64_t m_limit,m_count=0,m_pages=0,m_writtenPages=0,m_rows;uint32_t m_width;Stats m_stats{};
    bool m_registered=false;
    bool m_frozen=false,m_sealed=false,m_failed=false,m_visiting=false;
    static void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
    void Check(){m_resources.CheckFast();Require(!m_failed,"mutable_rows_failed");}
    static std::string Digest(const Slot& slot){analysis::Sha256Builder hash;hash.Update(slot.bytes.data()+HeaderBytes,PageBytes-HeaderBytes);return hash.FinalHex();}
    void Flush(uint64_t id,Slot& slot){
        if(!slot.dirty)return;Check();Header header;std::memcpy(&header,slot.bytes.data(),sizeof(header));const auto hash=Digest(slot);std::memcpy(header.checksum,hash.data(),64);std::memcpy(slot.bytes.data(),&header,sizeof(header));
        if(!m_file.is_open())Require(!std::filesystem::exists(analysis::AnalysisIoPath(m_path)),"mutable_rows_exists");
        m_resources.BeforeFileGrowth(m_path,(id+1)*PageBytes);m_registered=true;m_writtenPages=std::max(m_writtenPages,id+1);
        if(!m_file.is_open()){m_file.open(analysis::AnalysisIoPath(m_path),std::ios::in|std::ios::out|std::ios::binary|std::ios::trunc);Require(bool(m_file),"mutable_rows_open");}
        m_file.clear();m_file.seekp(std::streamoff(id*PageBytes));m_file.write(reinterpret_cast<const char*>(slot.bytes.data()),PageBytes);Require(bool(m_file),"mutable_rows_write");slot.dirty=false;++m_stats.pageWrites;
    }
    std::unique_ptr<Slot> TakeVictim(){Require(!m_lru.empty(),"mutable_rows_cache");++m_stats.evictions;++m_stats.evictionCandidates;const auto id=m_lru.front();m_lru.pop_front();auto victim=m_cache.find(id);Require(victim!=m_cache.end(),"mutable_rows_lru");Flush(victim->first,*victim->second);auto slot=std::move(victim->second);m_cache.erase(victim);return slot;}
    void Evict(){TakeVictim();}
    Slot& Get(uint64_t id,bool create=false){
        Check();Require(!m_visiting,"mutable_rows_reentrant_access");Require(id<m_pages||(create&&id==m_pages),"mutable_rows_page_id");auto found=m_cache.find(id);
        if(found==m_cache.end()){
            std::unique_ptr<Slot> slot;
            while((m_cache.size()+1)*PageCharge>m_limit)slot=TakeVictim();
            if(!slot){slot=std::make_unique<Slot>(m_resources);++m_stats.slotAllocations;}else ++m_stats.slotReuses;
            slot->dirty=false;
            if(id==m_pages){
                Require(create&&id<uint64_t(INT64_MAX)/PageBytes,"mutable_rows_offset");
                slot->bytes.fill(0);Header header;header.width=m_width;header.id=id;header.rows=m_rows;std::memcpy(slot->bytes.data(),&header,sizeof(header));slot->dirty=true;++m_pages;
            }else{
                Require(m_file.is_open(),"mutable_rows_missing_file");m_file.flush();Require(bool(m_file),"mutable_rows_flush");m_file.clear();m_file.seekg(std::streamoff(id*PageBytes));m_file.read(reinterpret_cast<char*>(slot->bytes.data()),PageBytes);
                Header header;std::memcpy(&header,slot->bytes.data(),sizeof(header));Require(bool(m_file)&&header.magic==Header{}.magic&&header.schema==1&&header.width==m_width&&header.id==id&&header.rows==m_rows,"mutable_rows_header");
                Require(Digest(*slot)==std::string(header.checksum,64),"mutable_rows_checksum");
            }
            m_lru.push_back(id);slot->lru=std::prev(m_lru.end());found=m_cache.emplace(id,std::move(slot)).first;++m_stats.pageLoads;
        }else{m_lru.splice(m_lru.end(),m_lru,found->second->lru);found->second->lru=std::prev(m_lru.end());}
        return *found->second;
    }
    template<class F> decltype(auto) Guard(F&& action){try{return action();}catch(const std::exception& e){m_failed=true;m_resources.Fail(e.what());throw;}catch(...){m_failed=true;m_resources.Fail("mutable_rows_failure");throw;}}
public:
    MutableRows(std::filesystem::path path,uint32_t width,ConversionResources& resources,uint64_t cacheBytes=2ull<<20)
        :m_path(std::move(path)),m_resources(resources),m_fixedWork(resources.Workspace(),16384),m_limit(cacheBytes),m_rows(width?(PageBytes-HeaderBytes)/width:0),m_width(width){
        Require(width>0&&width<=4096&&cacheBytes>=PageCharge&&cacheBytes<=32ull<<20,"mutable_rows_configuration");Require(!std::filesystem::exists(analysis::AnalysisIoPath(m_path)),"mutable_rows_exists");
    }
    ~MutableRows()=default;
    uint64_t Append(const void* row){Check();Require(!m_frozen&&!m_sealed&&row,"mutable_rows_append_state");return Guard([&]{Require(m_count<UINT64_MAX,"mutable_rows_count");const auto id=m_count;auto& slot=Get(id/m_rows,true);std::memcpy(slot.bytes.data()+HeaderBytes+(id%m_rows)*m_width,row,m_width);slot.dirty=true;++m_count;return id;});}
    void Read(uint64_t id,void* row){Check();Require(row&&id<m_count,"mutable_rows_index");Guard([&]{auto& slot=Get(id/m_rows);std::memcpy(row,slot.bytes.data()+HeaderBytes+(id%m_rows)*m_width,m_width);});}
    void Put(uint64_t id,const void* row){Check();Require(!m_sealed&&row&&id<m_count,"mutable_rows_update_state");Guard([&]{auto& slot=Get(id/m_rows);std::memcpy(slot.bytes.data()+HeaderBytes+(id%m_rows)*m_width,row,m_width);slot.dirty=true;});}
    void FreezeAppend(){Check();m_frozen=true;}
    void ClearCache(){Check();Require(!m_visiting,"mutable_rows_reentrant_access");Guard([&]{for(auto& entry:m_cache)Flush(entry.first,*entry.second);if(m_file.is_open()){m_file.flush();Require(bool(m_file),"mutable_rows_flush");}m_cache.clear();m_lru.clear();});}
    void Seal(){Check();Require(!m_sealed,"mutable_rows_already_sealed");m_frozen=true;m_sealed=true;}
    void ReduceCache(){Check();Require(!m_visiting,"mutable_rows_reentrant_access");m_limit=std::min<uint64_t>(m_limit,4*PageCharge);while(m_cache.size()*PageCharge>m_limit)Evict();}
    template<class F> void Visit(F&& visitor){
        Check();Require(m_sealed,"mutable_rows_not_sealed");Guard([&]{for(uint64_t first=0;first<m_count;){auto& slot=Get(first/m_rows);const auto n=std::min(m_rows,m_count-first);m_visiting=true;try{visitor(slot.bytes.data()+HeaderBytes,n);}catch(...){m_visiting=false;throw;}m_visiting=false;first+=n;}});
    }
    uint64_t Count()const{return m_count;}
    uint64_t LogicalBytes()const{return m_writtenPages*PageBytes;}
    bool HasDiskFile()const{return m_registered;}
    uint32_t Width()const{return m_width;}
    Stats Statistics()const{return m_stats;}
};
}
#endif
